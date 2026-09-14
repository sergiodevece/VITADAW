#include "vitadaw/application/DawApplication.h"
#include "vitadaw/audio/RealtimeAudioEngine.h"
#include "vitadaw/persistence/ProjectPersistence.h"
#include "vitadaw/processors/GainProcessor.h"
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <new>
#include <unistd.h>

namespace {
using namespace vitadaw;
using namespace persistence;
std::atomic<long> failAllocation{-1};
thread_local bool realtime{};
unsigned rtAllocations{}, rtDestructions{};
void check(bool yes, const char* message) {
    if (!yes) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}
std::string changed(std::string text, const std::string& from, const std::string& to) {
    auto at = text.find(from); check(at != std::string::npos, "mutation target exists");
    text.replace(at, from.size(), to); return text;
}
std::string bytes(std::string_view value) { return std::string(value); }
media::MediaFingerprint hash(std::string_view value) {
    return platform::files::fingerprint(std::as_bytes(std::span(value.data(), value.size())));
}
class Files final : public platform::files::IProjectFileIO {
public:
    std::map<std::filesystem::path, std::string> content;
    PersistencePhase fault{};
    platform::files::ReadResult read(const std::filesystem::path& path, std::size_t limit) override {
        auto i = content.find(path);
        if (i == content.end()) return {{PersistenceCode::fileNotFound, PersistencePhase::read}, {}};
        if (i->second.size() > limit) return {{PersistenceCode::fileTooLarge, PersistencePhase::read}, {}};
        auto view = std::as_bytes(std::span{i->second.data(), i->second.size()});
        return {{}, {view.begin(), view.end()}};
    }
    PersistenceResult replace(const std::filesystem::path& path, std::string_view data) override {
        if (fault != PersistencePhase::none && fault != PersistencePhase::durability)
            return {fault == PersistencePhase::replace ? PersistenceCode::atomicReplaceFailed : PersistenceCode::ioError, fault};
        content[path] = data;
        return fault == PersistencePhase::durability ? PersistenceResult{PersistenceCode::durabilityUncertain, fault} : PersistenceResult{};
    }
};

// Platform double owns decoded samples per candidate, never keyed globally by ID.
// Production compiler, processors, transport, smoothing, meters and render remain real.
class Engine final : public audio::IAudioEngineControl {
public:
    struct PCM { std::array<float, 480> samples; ~PCM() { if (realtime) ++rtDestructions; } };
    struct File final : audio::PreparedAudioFile {
        std::shared_ptr<PCM> pcm;
        File() : PreparedAudioFile({timeline::SampleRate{48000}, 1, {480}, {0.01}}), pcm(std::make_shared<PCM>()) {}
    };
    struct Owner { media::SourceId id; std::shared_ptr<PCM> pcm; };
    struct Plan final : audio::PreparedProcessingPlanChange {
        std::vector<Owner> owners;
        std::unique_ptr<audio::PreparedProcessingBundle> bundle;
        ~Plan() override { if (realtime) ++rtDestructions; }
    };
    Files& files;
    std::unique_ptr<Plan> active;
    audio::RealtimeAudioEngine rt;
    unsigned decodes{};
    bool failPrepare{}, failCommit{}, failReconnect{};
    explicit Engine(Files& f) : files(f) {}
    ~Engine() override { rt.deviceUnavailable(); rt.configure(audio::PreparedProjectView{}); active.reset(); }
    audio::AudioFilePreparationResult prepareWav(const std::filesystem::path& path) override {
        auto read = files.read(path, 1024);
        if (!read.result.success()) return {nullptr, {}, std::move(read.result)};
        ++decodes;
        auto result = std::make_unique<File>();
        result->pcm->samples.fill(read.bytes[0] == std::byte{'A'} ? 0.25F : -0.5F);
        result->media = {path, {}, platform::files::fingerprint(read.bytes)};
        return {std::move(result), {}};
    }
    audio::StructuralPlanPreparationResult build(const audio::ProcessingPlanSpecification& spec, std::vector<Owner> owners) {
        if (failPrepare) return {nullptr, "prepare fault"};
        auto plan = std::make_unique<Plan>(); plan->owners = std::move(owners);
        std::vector<audio::PreparedSourceView> views;
        for (const auto& o : plan->owners) views.push_back({o.id, {{o.pcm->samples.data(), nullptr}}, 1, {480}, timeline::SampleRate{48000}, media::AudioChannelLayout::mono});
        auto prepared = audio::prepareProcessingPlanFromSources(spec, views, 64);
        if (!prepared.success()) return {nullptr, std::move(prepared.errorMessage)};
        plan->bundle = std::move(prepared.prepared); return {std::move(plan), {}};
    }
    audio::StructuralPlanPreparationResult prepareProcessingPlan(const audio::ProcessingPlanSpecification& spec) override {
        return build(spec, active ? active->owners : std::vector<Owner>{});
    }
    audio::StructuralPlanPreparationResult prepareProcessingPlanWithAudio(const audio::ProcessingPlanSpecification& spec,
        media::SourceId id, audio::PreparedAudioFilePtr file) override {
        auto owners = active ? active->owners : std::vector<Owner>{};
        owners.push_back({id, static_cast<File*>(file.get())->pcm});
        return build(spec, std::move(owners));
    }
    audio::StructuralPlanPreparationResult prepareProjectReplacement(const audio::ProcessingPlanSpecification& spec,
        std::vector<audio::PreparedSourceAudio> files) override {
        std::vector<Owner> owners;
        for (auto& f : files) owners.push_back({f.id, static_cast<File*>(f.audio.get())->pcm});
        return build(spec, std::move(owners));
    }
    bool commitPreparedProcessingPlan(audio::PreparedProcessingPlanChangePtr next, audio::AudioFileCommitAction commit) noexcept override {
        if (failCommit) return false;
        rt.deviceStopped(); // No concurrent worker in this double: quiescent here.
        std::unique_ptr<Plan> candidate{static_cast<Plan*>(next.release())};
        rt.configure(candidate->bundle->plan, candidate->bundle->runtime);
        active.swap(candidate); commit.execute();
        if (failReconnect) rt.deviceError();
        else { rt.deviceInitialising(); rt.processBlock({nullptr, 0, 0}, timeline::SampleRate{48000}); }
        return true;
    }
    bool tryUpdateTrackMix(tracks::TrackId id, mixer::PreparedTrackMixState m, audio::PreparedAudibilityState a) noexcept override { return rt.tryUpdateTrackMix(id,m,a); }
    bool tryUpdateBusMix(routing::BusId id, mixer::PreparedBusMixState m, audio::PreparedAudibilityState a) noexcept override { return rt.tryUpdateBusMix(id,m,a); }
    bool tryUpdateSendMix(routing::SendId id, mixer::PreparedSendMixState m) noexcept override { return rt.tryUpdateSendMix(id,m); }
    bool tryUpdateMasterMix(mixer::PreparedMasterMixState m) noexcept override { return rt.tryUpdateMasterMix(m); }
    audio::AudioControlRequestResult tryRequestPlay() noexcept override { return rt.tryRequestPlay(); }
    audio::AudioControlRequestResult tryRequestStop() noexcept override { return rt.tryRequestStop(); }
    audio::RealtimeTransportSnapshot transportSnapshot() const noexcept override { return rt.transportSnapshot(); }
    mixer::MeterSnapshot meterSnapshot() const noexcept override { return rt.meterSnapshot(); }
    float render() {
        check(rt.tryRequestPlay().accepted, "Play prepared candidate");
        std::array<float, 1024> l{}, r{}; std::array<float*, 2> out{l.data(), r.data()};
        realtime = true; rt.processBlock({out.data(), 2, 1024}, timeline::SampleRate{48000}); realtime = false;
        check(rtAllocations == 0 && rtDestructions == 0, "loaded processBlock has no allocations/destructions");
        return l[10];
    }
};

project::ProjectState complex(const std::filesystem::path& mediaPath = "/session/media/a.wav") {
    project::ProjectState p{timeline::SampleRate{48000}, "Persistencia ñ"};
    auto t = p.addAudioTrack("Mono"), stereo = p.addAudioTrack("Stereo", media::AudioChannelLayout::stereo);
    auto imported = p.importAudioToTrack(t, {mediaPath, {}, hash("A")}, {480}, timeline::SampleRate{48000}, media::AudioChannelLayout::mono);
    for (int i = 1; i < 10; ++i) check(p.addClip(t, imported.source, {i * 24}, {240.5}, {12.25}).isValid(), "shared overlapping clip");
    auto b = p.addBus("Music"), aux = p.addBus("Aux");
    check(p.setTrackOutputDestination(t, routing::OutputDestination::toBus(b)), "track routing");
    check(p.setTrackOutputDestination(stereo, routing::OutputDestination::toBus(b)), "stereo routing");
    check(p.addSend(t, aux, routing::SendTapPoint::preFaderPrePan, {{-12}, false}).isValid(), "track send");
    check(p.addSend(b, aux, routing::SendTapPoint::postFaderPostPan, {{-6}, false}).isValid(), "bus send");
    auto gain = p.addProcessor(t, {processors::internalGainProcessorType});
    check(gain.isValid() && p.setProcessorParameter(gain, processors::gainParameterId, -3), "processor parameters");
    check(p.addProcessor(b, {processors::internalGainProcessorType}).isValid(), "bus insert");
    check(p.addProcessor(processors::MasterTarget{}, {processors::internalGainProcessorType}).isValid(), "master insert");
    check(p.setTrackMix(t, {{0},{0},false,true}), "selected mono");
    check(p.setTrackMix(stereo, {{-12},{0.375F},true,true}), "stereo mixer state");
    check(p.setBusMix(aux, {{-9},{-0.25F},false,false}), "bus mixer state");
    check(p.setMasterMix({{-6}}), "master state");
    return p;
}
std::string encode(const project::ProjectState& p, const std::filesystem::path& path = "/session/project.vitadaw") {
    auto encoded = serializeProject(p, path);
    if (!encoded.result.success()) std::cerr << "encode " << codeName(encoded.result.code) << " " << encoded.result.jsonPointer << '\n';
    check(encoded.result.success(), "serialize valid model"); return encoded.bytes;
}
void codecTests() {
    const auto p = complex(); const auto original = encode(p);
    check(original == encode(p), "deterministic bytes");
    auto loaded = deserializeProject(original); check(loaded.result.success(), "complex deserialize");
    check(encode(*loaded.project) == original, "full canonical model equality including every field/counter");
    check(loaded.project->tracks()[0].clips == p.tracks()[0].clips, "fractional clip values exact");
    check(loaded.project->sources()[0].sampleRate == p.sources()[0].sampleRate, "source clock exact");
    auto dto = p.documentData();
    constexpr std::uint64_t big = 9007199254740993ULL;
    for (auto& t : dto.tracks) { t.id.value += big; for(auto& c:t.clips){c.id.value+=big; c.source.value+=big;}
        for(auto& i:t.inserts.processors)i.id.value+=big; }
    for(auto& s:dto.sources)s.id.value+=big;
    for(auto& b:dto.routing.buses){b.id.value+=big;for(auto&i:b.inserts.processors)i.id.value+=big;}
    for(auto& r:dto.routing.trackRoutes){r.track.value+=big;if(r.destination.kind==routing::DestinationKind::bus)r.destination.bus.value+=big;}
    for(auto& s:dto.routing.sends){s.id.value+=big;s.destination.value+=big;std::visit([&](auto& id){id.value+=big;},s.source);}
    for(auto& i:dto.masterInserts.processors)i.id.value+=big;
    dto.nextTrackId.value+=big+17;dto.nextSourceId.value+=big+18;dto.nextClipId.value+=big+19;
    dto.nextProcessorId.value+=big+20;dto.routing.nextBusId.value+=big+21;dto.routing.nextSendId.value+=big+22;
    auto high = project::ProjectState::fromDocumentData(dto); check(bool(high), "large ID model");
    auto round = deserializeProject(encode(*high)); check(round.result.success(), "large IDs round trip");
    check(encode(*round.project)==encode(*high) && round.project->documentData().nextClipId == dto.nextClipId,
          "exact uint64 >2^53 and non-max+1 counters");
    auto reject = [&](std::string input, PersistenceCode code) {
        auto result=deserializeProject(input); if(result.result.code!=code)std::cerr<<codeName(result.result.code)<<" expected "<<codeName(code)<<'\n';
        check(result.result.code==code && !result.project, "corruption rejected explicitly");
    };
    reject(original.substr(0, original.size()/2), PersistenceCode::parseError);
    reject(changed(original,"VitaDAWProject","WrongProject"), PersistenceCode::schemaValidationFailed);
    reject(changed(original,"\"schemaVersion\": 1","\"schemaVersion\": 2"), PersistenceCode::unsupportedSchema);
    reject(changed(original,"\"schemaVersion\": 1","\"schemaVersion\": 1,\"schemaVersion\":1"), PersistenceCode::schemaValidationFailed);
    reject(changed(original,"\"schemaVersion\": 1","\"schemaVersion\": 1,\"futureField\":true"), PersistenceCode::schemaValidationFailed);
    reject(changed(original,"\"id\": \"1\"","\"id\": \"01\""), PersistenceCode::schemaValidationFailed);
    reject(changed(original,"\"id\": \"1\"","\"id\": \"18446744073709551616\""), PersistenceCode::schemaValidationFailed);
    reject(changed(original,"\"clip\": \"11\"","\"clip\": \"1\""), PersistenceCode::semanticValidationFailed);
    reject(changed(original,"\"sourceId\": \"1\"","\"sourceId\": \"999\""), PersistenceCode::semanticValidationFailed);
    reject(changed(original,"\"durationProjectFrames\": 480.0","\"durationProjectFrames\": 999.0"), PersistenceCode::semanticValidationFailed);
    reject(changed(original,"\"mono\"","\"surround\""), PersistenceCode::schemaValidationFailed);
    reject(changed(original,"preFaderPrePan","preWhatever"), PersistenceCode::schemaValidationFailed);
    reject(changed(original,"internal.gain","unknown.plugin"), PersistenceCode::processorUnavailable);
    reject(changed(original,"\"value\": 0.0","\"value\": 1000.0"), PersistenceCode::semanticValidationFailed);
    reject(changed(original,"\"sampleRateHz\": 48000.0","\"sampleRateHz\": 0.0"), PersistenceCode::semanticValidationFailed);
    reject(changed(original,"\"sampleRateHz\": 48000.0","\"sampleRateHz\": 1e9999"), PersistenceCode::parseError);
    reject(std::string(33,'[')+"0"+std::string(33,']'), PersistenceCode::capacityExceeded);
    reject(std::string(maximumDocumentBytes+1,' '), PersistenceCode::fileTooLarge);
    reject("{\"tracks\":["+std::string("0,").append(0,' ')+"0]}", PersistenceCode::schemaValidationFailed);
    std::string excessive="{\"tracks\":[0";for(int i=1;i<257;++i)excessive+=",0";excessive+="]}";
    reject(excessive, PersistenceCode::capacityExceeded);
    reject(changed(original,"Persistencia ñ",std::string(4097,'x')), PersistenceCode::capacityExceeded);
    auto bad = p.documentData(); bad.tracks[0].clips[1].id=bad.tracks[0].clips[0].id;
    check(!project::ProjectState::fromDocumentData(bad), "duplicate clip ID rejected");
    bad=p.documentData();bad.routing.buses[1].outputDestination=routing::OutputDestination::toBus(bad.routing.buses[0].id);
    check(!project::ProjectState::fromDocumentData(bad), "cycle combining bus output/send rejected");
    check(encode(p,"/other/deep/project.vitadaw").find("../../session/media/a.wav")!=std::string::npos, "Save As rebases paths");
    for (const char* name : {"minimal-project-v1.vitadaw", "full-project-v1.vitadaw"}) {
        std::ifstream file(std::filesystem::path{VITADAW_FIXTURES}/name);
        std::string fixture{std::istreambuf_iterator<char>(file), {}};
        auto r=deserializeProject(fixture);check(r.result.success(), "fixture loads");
        if(std::string_view(name).starts_with("minimal"))check(encode(*r.project)==fixture, "golden minimal exact bytes");
    }
    for (auto [name, code] : {std::pair{"future-version.vitadaw", PersistenceCode::unsupportedSchema},
        {"invalid-duplicate-key.vitadaw", PersistenceCode::schemaValidationFailed},
        {"invalid-truncated.vitadaw", PersistenceCode::parseError}}) {
        std::ifstream file(std::filesystem::path{VITADAW_FIXTURES}/name);
        reject({std::istreambuf_iterator<char>(file), {}}, code);
    }
}

void sessionTests() {
    Files f; f.content["/session/media/a.wav"]="A";f.content["/session/media/b.wav"]="B";
    auto a=complex(); auto b=complex("/session/media/b.wav");
    auto data=b.documentData();data.sources[0].media.fingerprint=hash("B");b=*project::ProjectState::fromDocumentData(std::move(data));
    f.content["/session/a.vitadaw"]=encode(a);f.content["/session/b.vitadaw"]=encode(b);
    Engine e{f};application::DawApplication app{e,timeline::SampleRate{44100},f};
    auto invoke=[&](commands::Command c, PersistenceCode expected=PersistenceCode::success){
        auto r=app.handle(c);check(r.persistence.code==expected, "persistence command status");
        check(r.status==(expected==PersistenceCode::success?commands::CommandStatus::accepted:commands::CommandStatus::rejected), "command acceptance");};
    invoke(commands::SaveProject{},PersistenceCode::savePathRequired);
    invoke(commands::LoadProject{"/session/a.vitadaw"});
    check(!app.session().dirty() && !app.canUndo() && app.project().sampleRate().hertz()==48000, "load clean session project rate");
    check(e.decodes==1 && e.active->owners.size()==1 && e.active->bundle->plan.clips.size()==10, "one Source ten Clips one decode/PCM");
    const auto sampleA=e.render(); app.synchroniseTransport();check(sampleA>0, "A renders actual PCM");
    std::weak_ptr<Engine::PCM> old=e.active->owners[0].pcm;
    invoke(commands::LoadProject{"/session/b.vitadaw"});
    check(old.expired(), "last old PCM owner destroyed after quiescence");
    check(e.render()<0, "B SourceId=1 renders B not A");app.synchroniseTransport();
    invoke(commands::MoveClip{{1},{50}});check(app.session().dirty(), "edit dirty");
    invoke(commands::LoadProject{"/session/a.vitadaw"},PersistenceCode::unsavedChanges);
    invoke(commands::Undo{});check(!app.session().dirty(), "Undo to saved clean");
    invoke(commands::SaveProject{});check(app.canRedo(), "Save keeps redo");
    invoke(commands::Redo{});check(app.session().dirty(), "Redo dirty");
    invoke(commands::SaveProjectAs{"/other/b.vitadaw"});check(!app.session().dirty()&&app.canUndo(), "Save As preserves history");
    invoke(commands::Undo{});check(app.session().dirty(), "Undo after Save dirty");
    auto before=encode(app.project());const auto token=app.history().currentStateToken();const auto saved=app.session().savedStateToken;
    auto* plan=e.active.get();const auto historySize=app.history().size(); const auto sessionPath=app.session().projectFilePath;
    auto unchanged=[&]{check(encode(app.project())==before&&e.active.get()==plan&&app.history().size()==historySize&&
        app.history().currentStateToken()==token&&app.session().savedStateToken==saved&&app.session().projectFilePath==sessionPath,"failure rollback complete");};
    for(auto phase:{PersistencePhase::tempCreate,PersistencePhase::write,PersistencePhase::flush,PersistencePhase::replace,PersistencePhase::durability}){
        f.fault=phase; auto oldBytes=f.content["/other/b.vitadaw"];
        invoke(commands::SaveProject{},phase==PersistencePhase::durability?PersistenceCode::durabilityUncertain:
            phase==PersistencePhase::replace?PersistenceCode::atomicReplaceFailed:PersistenceCode::ioError);
        unchanged();if(phase!=PersistencePhase::durability)check(f.content["/other/b.vitadaw"]==oldBytes,"pre-replace old bytes intact");
    }f.fault={};
    f.content["/bad.vitadaw"]="{";invoke(commands::LoadProject{"/bad.vitadaw",true},PersistenceCode::parseError);unchanged();
    f.content["/bad.vitadaw"]=changed(encode(a),"internal.gain","unknown.plugin");
    invoke(commands::LoadProject{"/bad.vitadaw",true},PersistenceCode::processorUnavailable);unchanged();
    f.content.erase("/session/media/a.wav");invoke(commands::LoadProject{"/session/a.vitadaw",true},PersistenceCode::missingMedia);unchanged();
    f.content["/session/media/a.wav"]="changed";invoke(commands::LoadProject{"/session/a.vitadaw",true},PersistenceCode::mediaChanged);unchanged();
    f.content["/session/media/a.wav"]="A";
    e.failPrepare=true;invoke(commands::LoadProject{"/session/a.vitadaw",true},PersistenceCode::preparationFailed);unchanged();e.failPrepare=false;
    e.failCommit=true;invoke(commands::LoadProject{"/session/a.vitadaw",true},PersistenceCode::preparationFailed);unchanged();e.failCommit=false;
    check(e.render()<0,"active audio still usable after all failures");app.synchroniseTransport();
    // Final source fails after earlier PCM was staged.
    auto multi=a;check(multi.importAudioToTrack({1},{"/missing.wav",{},hash("A")},{480},timeline::SampleRate{48000},media::AudioChannelLayout::mono).source.isValid(),"second source");
    f.content["/bad.vitadaw"]=encode(multi);invoke(commands::LoadProject{"/bad.vitadaw",true},PersistenceCode::missingMedia);unchanged();
    // Relative path wins when a project tree is moved; fallback when relative absent.
    f.content["/moved/a.vitadaw"]=f.content["/session/a.vitadaw"];f.content["/moved/media/a.wav"]="A";
    invoke(commands::LoadProject{"/moved/a.vitadaw",true});check(app.project().sources()[0].media.originalPath=="/moved/media/a.wav","moved relative resolution");
    f.content.erase("/moved/media/a.wav");invoke(commands::LoadProject{"/moved/a.vitadaw"});
    check(app.project().sources()[0].media.originalPath=="/session/media/a.wav","absolute fallback");
    // Save retains imported fingerprint even when media changed on disk.
    f.content["/session/media/a.wav"]="B";invoke(commands::SaveProject{});
    check(deserializeProject(f.content["/moved/a.vitadaw"]).project->sources()[0].media.fingerprint==hash("A"),"Save never rehashes old PCM");
    f.content["/session/media/a.wav"]="A";
    invoke(commands::Play{});
    invoke(commands::SaveProject{},PersistenceCode::transportMustBeStopped);
    invoke(commands::LoadProject{"/session/b.vitadaw",true},PersistenceCode::transportMustBeStopped);
    invoke(commands::Stop{}); e.render();app.synchroniseTransport();
    e.failReconnect=true;invoke(commands::LoadProject{"/session/b.vitadaw"});
    check(!app.session().dirty()&&e.rt.deviceState()==audio::DeviceProcessingState::error&&app.transport().position.value==0,
        "reconnect failure after commit keeps loaded document clean and stopped");
}

void nativeFiles() {
#ifdef __APPLE__
    std::array<char,64> pattern{};std::string prefix="/tmp/vitadaw-persistence-XXXXXX";std::copy(prefix.begin(),prefix.end(),pattern.begin());
    const auto* name=::mkdtemp(pattern.data());check(name,"temp test directory");std::filesystem::path directory{name};
    const auto path=directory/"project.vitadaw";
    platform::files::NativeProjectFileIO io;check(io.replace(path,"old").success(),"native durable replace");
    for(auto phase:{PersistencePhase::tempCreate,PersistencePhase::write,PersistencePhase::flush,PersistencePhase::replace,PersistencePhase::durability}){
        platform::files::NativeProjectFileIO faulty{phase};auto r=faulty.replace(path,"new");check(!r.success(),"native fault delivered");
        auto read=io.read(path,100);check(read.result.success(),"read survives save failure");
        const std::string value(reinterpret_cast<const char*>(read.bytes.data()),read.bytes.size());
        check(value==(phase==PersistencePhase::durability?"new":"old"),"native atomic semantics");
        check(std::distance(std::filesystem::directory_iterator(directory),std::filesystem::directory_iterator{})==1,"temporary file cleanup");
    }
    check(io.read(path,2).result.code==PersistenceCode::fileTooLarge,"native bounded read");
    check(platform::files::verifyFingerprint(path,hash("new")).success(),"streaming SHA256");
    auto snapshot = io.read(path, 100);
    check(io.replace(path, "new").success(), "replace with identical bytes");
    check(platform::files::verifyFingerprint(path,hash("new"), &snapshot.identity).code==PersistenceCode::mediaChanged,
          "replacement during decode rejected even if final hash matches");
    check(platform::files::verifyFingerprint(path,hash("old")).code==PersistenceCode::mediaChanged,"changed fingerprint");
    check(hash("abc").sha256=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","SHA256 known vector");
    std::filesystem::remove(path);std::filesystem::remove(directory);
#endif
}
void allocationRollback() {
    Files f;f.content["/session/media/a.wav"]="A";f.content["/load.vitadaw"]=encode(complex());
    Engine e{f};application::DawApplication app{e,timeline::SampleRate{48000},f};
    check(app.handle(commands::LoadProject{"/load.vitadaw"}).status==commands::CommandStatus::accepted,"baseline allocation test");
    const auto token=app.history().currentStateToken();auto* plan=e.active.get();
    const commands::Command command=commands::LoadProject{"/load.vitadaw"};
    unsigned failures{};bool completed{};
    for(long at=0;at<20000;++at){
        failAllocation=at;auto result=app.handle(command);failAllocation=-1;
        if(result.status==commands::CommandStatus::accepted){completed=true;break;}
        ++failures;check(e.active.get()==plan&&app.history().currentStateToken()==token&&!app.session().dirty(),"allocation exception preserves active document");
    }
    check(completed&&failures>100,"allocation sweep reaches successful noexcept commit");
    std::cout<<"Load allocation fault positions checked: "<<failures<<'\n';
}
}
void* operator new(std::size_t bytes) {
    if(realtime)++rtAllocations;
    auto at=failAllocation.load();if(at>=0){if(at==0){failAllocation=-1;throw std::bad_alloc{};}failAllocation=at-1;}
    if(auto*p=std::malloc(bytes?bytes:1))return p;throw std::bad_alloc{};
}
void operator delete(void* p) noexcept {if(realtime&&p)++rtDestructions;std::free(p);}
void* operator new[](std::size_t n){return ::operator new(n);}
void operator delete[](void*p)noexcept{::operator delete(p);}
int main(){codecTests();sessionTests();nativeFiles();allocationRollback();std::cout<<"Project persistence passed\n";}
