#include "vitadaw/persistence/ProjectPersistence.h"
#include "vitadaw/processors/GainProcessor.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <set>
#include <limits>

namespace vitadaw::persistence {
namespace {
// JSON containers are capped independently; input/output <=16 MiB, strings and
// bounded DTOs occupy the remaining headroom of the 128 MiB document envelope.
thread_local std::size_t jsonBytes{};
template<class T> struct BudgetAllocator {
    using value_type = T;
    BudgetAllocator() = default;
    template<class U> BudgetAllocator(const BudgetAllocator<U>&) {}
    T* allocate(std::size_t count) {
        constexpr auto budget = maximumDocumentMemory / 4;
        if (count > (budget - jsonBytes) / sizeof(T)) throw std::bad_alloc{};
        auto* p = std::allocator<T>{}.allocate(count); jsonBytes += count*sizeof(T); return p;
    }
    void deallocate(T* p, std::size_t count) noexcept {
        jsonBytes -= count*sizeof(T); std::allocator<T>{}.deallocate(p,count);
    }
    template<class U> bool operator==(const BudgetAllocator<U>&) const { return true; }
};
using J = nlohmann::basic_json<std::map,std::vector,std::string,bool,std::int64_t,
    std::uint64_t,double,BudgetAllocator>;
// nlohmann's general-purpose destructor flattens trees using a heap stack.
// Documents have already passed depth<=32: drain children first with a bounded
// call stack so normal document cleanup cannot allocate (including after commit).
struct DocumentTree {
    J value;
    static void drain(J& node) noexcept {
        if (node.is_object()) {
            auto& object = node.get_ref<J::object_t&>();
            for (auto& [key, child] : object) { (void)key; drain(child); }
            object.clear();
        } else if (node.is_array()) {
            auto& array = node.get_ref<J::array_t&>();
            for (auto& child : array) drain(child);
            array.clear();
        }
    }
    ~DocumentTree() { drain(value); }
};
struct Invalid { PersistenceCode code; std::string pointer; };
[[noreturn]] void invalid(std::string pointer, PersistenceCode code = PersistenceCode::schemaValidationFailed) {
    // Only publish locations we can name exactly. Leaf validation without a
    // tracked document path deliberately leaves optional JSON Pointer absent.
    constexpr std::array<std::string_view, 11> roots{"format", "schemaVersion", "writerAppVersion",
        "projectSettings", "nextIds", "sources", "tracks", "routing", "buses", "sends", "master"};
    if (!pointer.empty() && std::find(roots.begin(), roots.end(), pointer) != roots.end()) pointer.insert(0, "/");
    else pointer.clear();
    throw Invalid{code,std::move(pointer)};
}
void fields(const J& j, std::initializer_list<const char*> required,
            std::initializer_list<const char*> optional = {}) {
    if (!j.is_object()) invalid("object");
    for (auto key : required) if (!j.contains(key)) invalid(key);
    for (const auto& [key,value] : j.items()) {
        (void)value;
        if (std::find(required.begin(), required.end(), key) == required.end() &&
            std::find(optional.begin(), optional.end(), key) == optional.end()) invalid(key);
    }
}
void array(const J& j, std::size_t limit) {
    if (!j.is_array()) invalid("array");
    if (j.size() > limit) invalid("array",PersistenceCode::capacityExceeded);
}
std::string string(const J& j) {
    if (!j.is_string()) invalid("string");
    const auto& s = j.get_ref<const std::string&>();
    if (s.size() > 4096 || s.find('\0') != std::string::npos) invalid("string");
    return s;
}
std::uint64_t integer(const J& j, bool zero = false) {
    const auto s = string(j);
    if (s.empty() || s.size() > 20 || (s.size()>1 && s[0]=='0') || s[0]<'0' || s[0]>'9') invalid("integer");
    std::uint64_t n{};
    auto [end,ec] = std::from_chars(s.data(),s.data()+s.size(),n);
    if (ec != std::errc{} || end != s.data()+s.size() || (!zero && n==0)) invalid("integer");
    return n;
}
double number(const J& j) {
    if (!j.is_number()) invalid("number");
    const auto n=j.get<double>(); if (!std::isfinite(n)) invalid("number"); return n;
}
float real(const J& j) {
    const auto n=number(j);
    if (std::abs(n)>std::numeric_limits<float>::max()) invalid("float");
    return static_cast<float>(n);
}
bool boolean(const J& j) { if (!j.is_boolean()) invalid("boolean"); return j.get<bool>(); }
std::string pathString(const std::filesystem::path& p) {
    const auto s=p.generic_u8string(); return {s.begin(),s.end()};
}
std::filesystem::path path(const J& j, bool relative) {
    const auto s=string(j);
    auto p=std::filesystem::path(std::u8string(s.begin(), s.end()));
    if (s.empty() || s.starts_with("~") || s.find("://")!=std::string::npos ||
        (relative ? p.is_absolute() : !p.is_absolute())) invalid("path");
    return p;
}
media::AudioChannelLayout layout(const J& j) {
    const auto s=string(j);
    if (s=="mono") return media::AudioChannelLayout::mono;
    if (s=="stereo") return media::AudioChannelLayout::stereo;
    invalid("layout");
}
const char* layoutName(media::AudioChannelLayout l) {
    return l==media::AudioChannelLayout::mono ? "mono" : "stereo";
}
routing::OutputDestination destination(const J& j) {
    const auto kind=string(j.at("kind"));
    if (kind=="master") { fields(j,{"kind"}); return routing::OutputDestination::master(); }
    if (kind=="bus") { fields(j,{"kind","id"}); return routing::OutputDestination::toBus({integer(j.at("id"))}); }
    invalid("destination");
}
J destination(const routing::OutputDestination& value) {
    if (value.kind==routing::DestinationKind::master) return J{{"kind","master"}};
    return J{{"kind","bus"},{"id",std::to_string(value.bus.value)}};
}
processors::InsertChain chain(const J& j) {
    array(j,16); processors::InsertChain result;
    for (const auto& entry : j) {
        fields(entry,{"id","typeId","bypassed","parameters","serializedState"});
        processors::ProcessorState p;
        p.id={integer(entry.at("id"))}; p.type={string(entry.at("typeId"))};
        if (p.type.identifier != processors::internalGainProcessorType)
            invalid("typeId",PersistenceCode::processorUnavailable);
        p.bypassed=boolean(entry.at("bypassed"));
        if (string(entry.at("serializedState"))!="") invalid("serializedState");
        array(entry.at("parameters"),1);
        for (const auto& parameter : entry.at("parameters")) {
            fields(parameter,{"id","value"});
            const auto id=integer(parameter.at("id"));
            if (id>UINT32_MAX) invalid("parameter/id");
            p.parameters.push_back({{static_cast<std::uint32_t>(id)},real(parameter.at("value"))});
        }
        result.processors.push_back(std::move(p));
    }
    return result;
}
J chain(const processors::InsertChain& c) {
    J result=J::array();
    for (const auto& p : c.processors) {
        J params=J::array();
        auto ordered=p.parameters;
        std::sort(ordered.begin(),ordered.end(),[](auto a,auto b){return a.id<b.id;});
        for (auto param : ordered) params.push_back({{"id",std::to_string(param.id.value)},{"value",param.value}});
        result.push_back({{"id",std::to_string(p.id.value)},{"typeId",p.type.identifier},
            {"bypassed",p.bypassed},{"parameters",std::move(params)},{"serializedState",""}});
    }
    return result;
}
media::MediaReference reference(const J& j) {
    fields(j,{"kind","fingerprint"},{"relativePath","absoluteFallback"});
    if (string(j.at("kind"))!="localFile") invalid("media/kind");
    media::MediaReference m;
    if (j.contains("relativePath")) m.projectRelativePath=path(j.at("relativePath"),true);
    if (j.contains("absoluteFallback")) m.originalPath=path(j.at("absoluteFallback"),false);
    if (!m.isValid()) invalid("media/path");
    const auto& fp=j.at("fingerprint"); fields(fp,{"algorithm","digest","fileSizeBytes"});
    const auto digest=string(fp.at("digest"));
    if (string(fp.at("algorithm"))!="sha256" || digest.size()!=64 ||
        digest.find_first_not_of("0123456789abcdef")!=std::string::npos) invalid("fingerprint");
    m.fingerprint=media::MediaFingerprint{digest,integer(fp.at("fileSizeBytes"))}; return m;
}
J reference(const media::MediaReference& m, const std::filesystem::path& target) {
    if (!m.fingerprint || !m.originalPath.is_absolute()) invalid("media",PersistenceCode::semanticValidationFailed);
    J result{{"kind","localFile"},{"absoluteFallback",pathString(m.originalPath)},
        {"fingerprint",{{"algorithm","sha256"},{"digest",m.fingerprint->sha256},
        {"fileSizeBytes",std::to_string(m.fingerprint->fileSizeBytes)}}}};
    const auto relative=m.originalPath.lexically_relative(target.parent_path());
    if (!relative.empty()) result["relativePath"]=pathString(relative);
    static_cast<void>(reference(result)); return result;
}

// SAX pass rejects duplicate keys/depth/counts before allocating a DOM. A single
// lexical string is still bounded by the 16 MiB input, then rejected at 4 KiB.
struct Guard : nlohmann::json_sax<J> {
    struct Level { bool object; std::set<std::string> keys; std::size_t count{}; std::size_t limit; };
    std::vector<Level> levels;
    std::string lastKey;
    std::size_t nodes{}, totalStrings{};
    PersistenceCode failure{PersistenceCode::parseError};
    bool value() { if (++nodes>500000) {failure=PersistenceCode::capacityExceeded;return false;}
        if (!levels.empty() && !levels.back().object && ++levels.back().count>levels.back().limit) {
            failure=PersistenceCode::capacityExceeded;return false;} return true; }
    bool null() override { return value(); }
    bool boolean(bool) override { return value(); }
    bool number_integer(number_integer_t) override { return value(); }
    bool number_unsigned(number_unsigned_t) override { return value(); }
    bool number_float(number_float_t n,const string_t&) override {return std::isfinite(n)&&value();}
    bool string(string_t& s) override {
        totalStrings+=s.size();
        if (s.size()>4096 || totalStrings>8*1024*1024) { failure=PersistenceCode::capacityExceeded;return false; }
        return value();
    }
    bool binary(binary_t&) override {return false;}
    bool start_object(std::size_t) override {
        if (levels.size()>=32 || !value()) {failure=PersistenceCode::capacityExceeded;return false;}
        levels.push_back({true,{},0,64}); return true;
    }
    bool key(string_t& s) override {
        if (s.size()>128 || levels.back().keys.size()>=64) {failure=PersistenceCode::capacityExceeded;return false;}
        if (!levels.back().keys.insert(s).second) {failure=PersistenceCode::schemaValidationFailed;return false;}
        lastKey=s; return true;
    }
    bool end_object() override { levels.pop_back(); return true; }
    bool start_array(std::size_t) override {
        if (levels.size()>=32 || !value()) {failure=PersistenceCode::capacityExceeded;return false;}
        const auto limit=lastKey=="tracks" ? 256u : lastKey=="sources" ? 2048u :
            lastKey=="clips" ? 4096u : lastKey=="buses" ? 64u : lastKey=="sends" ? 1024u :
            lastKey=="tempoEvents" || lastKey=="signatureEvents" ? 4096u :
            lastKey=="inserts" ? 16u : lastKey=="parameters" ? 1u : 32768u;
        levels.push_back({false,{},0,limit}); return true;
    }
    bool end_array() override {levels.pop_back();return true;}
    bool parse_error(std::size_t,const std::string&,const nlohmann::detail::exception&) override {return false;}
};
using Migration = void(*)(J&);
ProjectDocument decode(const J&);
void migrateV1ToV2(J& root) {
    // Validate the complete legacy document BEFORE adding any defaults.
    auto legacy = decode(root);
    if (!project::ProjectState::fromDocumentData(std::move(legacy.model)))
        invalid("v1", PersistenceCode::semanticValidationFailed);
    root["musicalTime"] = {{"ppq",musical::ppq},{"nextTempoEventId","2"},
        {"nextTimeSignatureEventId","2"},
        {"tempoEvents",J::array({{{"id","1"},{"tick","0"},{"bpm",120.0},{"curve","step"}}})},
        {"signatureEvents",J::array({{{"id","1"},{"barIndex","0"},{"numerator",4},{"denominator",4}}})}};
    root["schemaVersion"] = 2u;
}
void migrateV2ToV3(J& root) {
    auto legacy = decode(root);
    if (!project::ProjectState::fromDocumentData(std::move(legacy.model)))
        invalid("v2", PersistenceCode::semanticValidationFailed);
    root["loopRange"] = nullptr;
    root["schemaVersion"] = 3u;
}
constexpr std::array<Migration,2> migrations{migrateV1ToV2,migrateV2ToV3};
void migrate(J& root) {
    if (!root.contains("schemaVersion") || !root.at("schemaVersion").is_number_unsigned()) invalid("schemaVersion");
    auto version=root.at("schemaVersion").get<std::uint64_t>();
    if (!version || version>ProjectDocument::currentSchemaVersion) invalid("schemaVersion",PersistenceCode::unsupportedSchema);
    while (version<ProjectDocument::currentSchemaVersion) {
        if (version-1>=migrations.size()) invalid("schemaVersion",PersistenceCode::unsupportedSchema);
        migrations[version-1](root); ++version;
    }
}
ProjectDocument decode(const J& root) {
    const auto version = root.at("schemaVersion").get<unsigned>();
    if (version == 1)
    fields(root,{"format","schemaVersion","projectSettings","nextIds","sources","tracks","routing","buses","sends","master"},
           {"writerAppVersion"});
    else if (version == 2)
        fields(root,{"format","schemaVersion","projectSettings","nextIds","sources","tracks","routing","buses","sends","master","musicalTime"},
               {"writerAppVersion"});
    else if (version == 3)
        fields(root,{"format","schemaVersion","projectSettings","nextIds","sources","tracks","routing","buses","sends","master","musicalTime","loopRange"},
               {"writerAppVersion"});
    else invalid("schemaVersion",PersistenceCode::unsupportedSchema);
    if (string(root.at("format"))!="VitaDAWProject") invalid("format");
    ProjectDocument doc;
    doc.schemaVersion=version;
    if (root.contains("writerAppVersion")) doc.writerAppVersion=string(root.at("writerAppVersion"));
    auto& data=doc.model;
    if (version >= 2) {
        const auto& m=root.at("musicalTime");
        fields(m,{"ppq","nextTempoEventId","nextTimeSignatureEventId","tempoEvents","signatureEvents"});
        if (!m.at("ppq").is_number_integer() || m.at("ppq") != musical::ppq) invalid("musicalTime/ppq");
        auto& map=data.musicalTime;
        map.tempo.nextId={integer(m.at("nextTempoEventId"))};
        map.signatures.nextId={integer(m.at("nextTimeSignatureEventId"))};
        array(m.at("tempoEvents"),musical::maximumEvents);
        array(m.at("signatureEvents"),musical::maximumEvents);
        map.tempo.events.clear(); map.signatures.events.clear();
        for (const auto& e:m.at("tempoEvents")) {
            fields(e,{"id","tick","bpm","curve"});
            const auto tick=integer(e.at("tick"),true);
            if (tick>musical::maximumCoordinate || string(e.at("curve"))!="step") invalid("musicalTime/tempoEvents");
            map.tempo.events.push_back({{integer(e.at("id"))},{static_cast<std::int64_t>(tick)},
                {number(e.at("bpm"))},musical::TempoCurve::step});
        }
        for (const auto& e:m.at("signatureEvents")) {
            fields(e,{"id","barIndex","numerator","denominator"});
            const auto bar=integer(e.at("barIndex"),true);
            if (bar>musical::maximumCoordinate || !e.at("numerator").is_number_integer() ||
                !e.at("denominator").is_number_integer() || e.at("numerator")<1 || e.at("denominator")<1 ||
                e.at("numerator")>32u || e.at("denominator")>64u)
                invalid("musicalTime/signatureEvents");
            map.signatures.events.push_back({{integer(e.at("id"))},{static_cast<std::int64_t>(bar)},
                {e.at("numerator").get<unsigned>(),e.at("denominator").get<unsigned>()}});
        }
    }
    if (version >= 3 && !root.at("loopRange").is_null()) {
        const auto& loop = root.at("loopRange");
        fields(loop,{"startTick","endTick"});
        const auto start = integer(loop.at("startTick"), true);
        const auto end = integer(loop.at("endTick"), true);
        if (start > musical::maximumCoordinate || end > musical::maximumCoordinate)
            invalid("loopRange");
        const musical::MusicalLoopRange decodedLoop{
            {static_cast<std::int64_t>(start)},
            {static_cast<std::int64_t>(end)}};
        if (!decodedLoop.isStructurallyValid()) invalid("loopRange");
        data.loopRange = decodedLoop;
    }
    const auto& settings=root.at("projectSettings"); fields(settings,{"name","sampleRateHz"});
    data.settings={string(settings.at("name")),timeline::SampleRate{number(settings.at("sampleRateHz"))}};
    const auto& ids=root.at("nextIds"); fields(ids,{"track","source","clip","bus","send","processorInstance"});
    data.nextTrackId={integer(ids.at("track"))}; data.nextSourceId={integer(ids.at("source"))};
    data.nextClipId={integer(ids.at("clip"))}; data.nextProcessorId={integer(ids.at("processorInstance"))};
    data.routing.nextBusId={integer(ids.at("bus"))}; data.routing.nextSendId={integer(ids.at("send"))};
    array(root.at("sources"),2048);
    for (const auto& s:root.at("sources")) {
        fields(s,{"id","media","frameCount","sampleRateHz","layout"});
        data.sources.push_back({{integer(s.at("id"))},reference(s.at("media")),{integer(s.at("frameCount"))},
            timeline::SampleRate{number(s.at("sampleRateHz"))},layout(s.at("layout"))});
    }
    array(root.at("tracks"),256);
    std::size_t totalClips{};
    for (const auto& t:root.at("tracks")) {
        fields(t,{"id","name","layout","clips","mix","inserts"});
        tracks::AudioTrack track;
        track.id={integer(t.at("id"))}; track.name=string(t.at("name")); track.layout=layout(t.at("layout"));
        const auto& mix=t.at("mix"); fields(mix,{"gainDb","pan","muted","solo"});
        track.mix={{real(mix.at("gainDb"))},{real(mix.at("pan"))},boolean(mix.at("muted")),boolean(mix.at("solo"))};
        track.inserts=chain(t.at("inserts")); array(t.at("clips"),4096);
        totalClips+=t.at("clips").size(); if(totalClips>32768) invalid("clips",PersistenceCode::capacityExceeded);
        for (const auto& c:t.at("clips")) {
            fields(c,{"id","sourceId","projectStartFrames","durationProjectFrames","sourceOffsetFrames"});
            const auto start=integer(c.at("projectStartFrames"),true);
            if(start>INT64_MAX) invalid("projectStartFrames");
            track.clips.push_back({{integer(c.at("id"))},{integer(c.at("sourceId"))},
                {static_cast<std::int64_t>(start)},{number(c.at("durationProjectFrames"))},{number(c.at("sourceOffsetFrames"))}});
        }
        data.tracks.push_back(std::move(track));
    }
    const auto& routing=root.at("routing"); fields(routing,{"trackOutputs"}); array(routing.at("trackOutputs"),256);
    for (const auto& r:routing.at("trackOutputs")) {
        fields(r,{"trackId","output"}); data.routing.trackRoutes.push_back({{integer(r.at("trackId"))},destination(r.at("output"))});
    }
    array(root.at("buses"),64);
    for (const auto& b:root.at("buses")) {
        fields(b,{"id","name","mix","output","inserts"});
        const auto& m=b.at("mix"); fields(m,{"gainDb","balance","muted","solo"});
        data.routing.buses.push_back({{integer(b.at("id"))},string(b.at("name")),
            {{real(m.at("gainDb"))},{real(m.at("balance"))},boolean(m.at("muted")),boolean(m.at("solo"))},
            destination(b.at("output")),chain(b.at("inserts"))});
    }
    array(root.at("sends"),1024);
    for (const auto& s:root.at("sends")) {
        fields(s,{"id","source","destinationBusId","tap","levelDb","muted"});
        const auto& src=s.at("source"); fields(src,{"kind","id"});
        routing::SendSource source;
        const auto kind=string(src.at("kind"));
        if(kind=="track") source=tracks::TrackId{integer(src.at("id"))};
        else if(kind=="bus") source=routing::BusId{integer(src.at("id"))}; else invalid("source/kind");
        const auto tap=string(s.at("tap"));
        if(tap!="preFaderPrePan" && tap!="postFaderPostPan") invalid("tap");
        data.routing.sends.push_back({{integer(s.at("id"))},source,{integer(s.at("destinationBusId"))},
            tap=="preFaderPrePan"?routing::SendTapPoint::preFaderPrePan:routing::SendTapPoint::postFaderPostPan,
            {{real(s.at("levelDb"))},boolean(s.at("muted"))}});
    }
    const auto& master=root.at("master"); fields(master,{"gainDb","inserts"});
    data.masterMix={{real(master.at("gainDb"))}}; data.masterInserts=chain(master.at("inserts"));
    return doc;
}
J encode(ProjectDocument doc,const std::filesystem::path& path) {
    auto& d=doc.model;
    J root{{"format","VitaDAWProject"},{"schemaVersion",doc.schemaVersion},{"writerAppVersion",doc.writerAppVersion},
        {"projectSettings",{{"name",d.settings.name},{"sampleRateHz",d.settings.sampleRate.hertz()}}},
        {"nextIds",{{"track",std::to_string(d.nextTrackId.value)},{"source",std::to_string(d.nextSourceId.value)},
        {"clip",std::to_string(d.nextClipId.value)},{"bus",std::to_string(d.routing.nextBusId.value)},
        {"send",std::to_string(d.routing.nextSendId.value)},{"processorInstance",std::to_string(d.nextProcessorId.value)}}}};
    const auto& map=d.musicalTime;
    J tempos=J::array(), signatures=J::array();
    // Validated maps have unique anchors in ascending order (therefore canonical).
    for (const auto& e:map.tempo.events) tempos.push_back({{"id",std::to_string(e.id.value)},
        {"tick",std::to_string(e.tick.value)},{"bpm",e.bpm.value},{"curve","step"}});
    for (const auto& e:map.signatures.events) signatures.push_back({{"id",std::to_string(e.id.value)},
        {"barIndex",std::to_string(e.bar.value)},{"numerator",e.signature.numerator},{"denominator",e.signature.denominator}});
    root["musicalTime"]={{"ppq",map.resolution},{"nextTempoEventId",std::to_string(map.tempo.nextId.value)},
        {"nextTimeSignatureEventId",std::to_string(map.signatures.nextId.value)},
        {"tempoEvents",std::move(tempos)},{"signatureEvents",std::move(signatures)}};
    root["loopRange"] = d.loopRange
        ? J{{"startTick",std::to_string(d.loopRange->start.value)},
            {"endTick",std::to_string(d.loopRange->end.value)}}
        : J(nullptr);
    for(auto key:{"sources","tracks","buses","sends"}) root[key]=J::array();
    std::sort(d.sources.begin(),d.sources.end(),[](const auto& a,const auto& b){return a.id<b.id;});
    for(const auto& s:d.sources) root["sources"].push_back({{"id",std::to_string(s.id.value)},
        {"media",reference(s.media,path)},{"frameCount",std::to_string(s.frameCount.value)},
        {"sampleRateHz",s.sampleRate.hertz()},{"layout",layoutName(s.layout)}});
    for(auto& t:d.tracks) {
        J clips=J::array();
        std::sort(t.clips.begin(),t.clips.end(),[](const auto& a,const auto& b){return a.projectStart.value==b.projectStart.value ? a.id<b.id:a.projectStart.value<b.projectStart.value;});
        for(const auto& c:t.clips) clips.push_back({{"id",std::to_string(c.id.value)},{"sourceId",std::to_string(c.source.value)},
            {"projectStartFrames",std::to_string(c.projectStart.value)},{"durationProjectFrames",c.duration.value},{"sourceOffsetFrames",c.sourceOffset.value}});
        root["tracks"].push_back({{"id",std::to_string(t.id.value)},{"name",t.name},{"layout",layoutName(t.layout)},
            {"clips",std::move(clips)},{"inserts",chain(t.inserts)},
            {"mix",{{"gainDb",t.mix.gain.value},{"pan",t.mix.pan.value},{"muted",t.mix.muted},{"solo",t.mix.solo}}}});
    }
    J routes=J::array();
    std::sort(d.routing.trackRoutes.begin(),d.routing.trackRoutes.end(),[](auto a,auto b){return a.track<b.track;});
    for(auto r:d.routing.trackRoutes) routes.push_back({{"trackId",std::to_string(r.track.value)},{"output",destination(r.destination)}});
    root["routing"]={{"trackOutputs",std::move(routes)}};
    for(const auto& b:d.routing.buses) root["buses"].push_back({{"id",std::to_string(b.id.value)},{"name",b.name},
        {"mix",{{"gainDb",b.mix.gain.value},{"balance",b.mix.balance.value},{"muted",b.mix.muted},{"solo",b.mix.solo}}},
        {"output",destination(b.outputDestination)},{"inserts",chain(b.inserts)}});
    std::sort(d.routing.sends.begin(),d.routing.sends.end(),[](auto a,auto b){return a.id<b.id;});
    for(const auto& s:d.routing.sends) {
        const bool track=std::holds_alternative<tracks::TrackId>(s.source);
        const auto id=track?std::get<tracks::TrackId>(s.source).value:std::get<routing::BusId>(s.source).value;
        root["sends"].push_back({{"id",std::to_string(s.id.value)},{"source",{{"kind",track?"track":"bus"},{"id",std::to_string(id)}}},
            {"destinationBusId",std::to_string(s.destination.value)},
            {"tap",s.tapPoint==routing::SendTapPoint::preFaderPrePan?"preFaderPrePan":"postFaderPostPan"},
            {"levelDb",s.mix.level.value},{"muted",s.mix.muted}});
    }
    root["master"]={{"gainDb",d.masterMix.gain.value},{"inserts",chain(d.masterInserts)}}; return root;
}
}
DocumentResult deserializeProject(std::string_view bytes) {
    try {
        if(bytes.size()>maximumDocumentBytes) return {{PersistenceCode::fileTooLarge,PersistencePhase::read},{}};
        Guard guard;
        if(!J::sax_parse(bytes,&guard)) return {{guard.failure,PersistencePhase::parse},{}};
        DocumentTree tree{J::parse(bytes)};
        auto& root=tree.value; migrate(root); auto doc=decode(root);
        auto project=project::ProjectState::fromDocumentData(std::move(doc.model));
        if(!project) return {{PersistenceCode::semanticValidationFailed,PersistencePhase::validate},{}};
        return {{},std::move(project)};
    } catch(const Invalid& e) { return {{e.code,PersistencePhase::validate,{},e.pointer},{}}; }
      catch(const std::bad_alloc&) { return {{PersistenceCode::capacityExceeded,PersistencePhase::parse},{}}; }
      catch(const J::exception&) { return {{PersistenceCode::schemaValidationFailed,PersistencePhase::parse},{}}; }
      catch(...) { return {{PersistenceCode::semanticValidationFailed,PersistencePhase::validate},{}}; }
}
SerializationResult serializeProject(const project::ProjectState& project,const std::filesystem::path& path) {
    try {
        if(!path.is_absolute()) return {{PersistenceCode::schemaValidationFailed,PersistencePhase::validate},{}};
        auto data=project.documentData();
        if(!project::ProjectState::fromDocumentData(data)) return {{PersistenceCode::semanticValidationFailed,PersistencePhase::validate},{}};
        DocumentTree tree{encode({ProjectDocument::currentSchemaVersion,"0.7.0",std::move(data)},path)};
        auto& root=tree.value;
        // Explicit schema validation on Save also catches unsupported media fields.
        static_cast<void>(decode(root));
        auto bytes=root.dump(2); bytes+='\n';
        if(bytes.size()>maximumDocumentBytes) return {{PersistenceCode::fileTooLarge,PersistencePhase::write},{}};
        Guard guard;
        if(!J::sax_parse(bytes,&guard)) return {{guard.failure,PersistencePhase::validate},{}};
        return {{},std::move(bytes)};
    } catch(const Invalid& e) { return {{e.code,PersistencePhase::validate,{},e.pointer},{}}; }
      catch(const std::bad_alloc&) { return {{PersistenceCode::capacityExceeded,PersistencePhase::validate},{}}; }
      catch(...) { return {{PersistenceCode::semanticValidationFailed,PersistencePhase::validate},{}}; }
}
} // namespace vitadaw::persistence
