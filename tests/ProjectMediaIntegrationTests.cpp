#include "vitadaw/platform/juce/JuceAudioDeviceAdapter.h"
#include "vitadaw/application/DawApplication.h"
#include "vitadaw/persistence/ProjectPersistence.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <array>
#include <atomic>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <new>
#include <optional>
#include <unistd.h>

namespace {
std::atomic<bool> countRealtimeAllocations{};
std::atomic<std::size_t> realtimeAllocations{};
}

void* operator new(std::size_t size) {
    if (countRealtimeAllocations.load(std::memory_order_relaxed))
        realtimeAllocations.fetch_add(1, std::memory_order_relaxed);
    if (auto* memory = std::malloc(size)) return memory;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace vitadaw::platform::juce_adapter {
class PersistenceIntegrationAccess {
public:
    static std::optional<JuceAudioDeviceAdapter::FileIdentity> createExclusiveTemporary(
        const std::filesystem::path& path, std::error_code& error) {
        auto temporary = JuceAudioDeviceAdapter::createExclusiveTemporaryFile(path, error);
        if (!temporary) return std::nullopt;
        static_cast<void>(::close(temporary->release()));
        return temporary->identity;
    }
    static bool closesExclusiveOnAbandon(const std::filesystem::path& path,
                                         std::error_code& error) {
        auto temporary = JuceAudioDeviceAdapter::createExclusiveTemporaryFile(path, error);
        if (!temporary) return false;
        const auto descriptor = temporary->descriptor_;
        temporary.reset(); // Models allocation failure before stream construction.
        errno = 0;
        return ::fcntl(descriptor, F_GETFD) == -1 && errno == EBADF;
    }
    static bool discard(JuceAudioDeviceAdapter& adapter,
                        const std::filesystem::path& temporary,
                        std::optional<JuceAudioDeviceAdapter::FileIdentity> temporaryIdentity,
                        const std::filesystem::path& published,
                        std::optional<JuceAudioDeviceAdapter::FileIdentity> publishedIdentity,
                        bool removePublished) {
        adapter.recordingTemporaryPath_ = temporary;
        adapter.recordingTemporaryIdentity_ = temporaryIdentity;
        adapter.recordingPublishedPath_ = published;
        adapter.recordingPublishedIdentity_ = publishedIdentity;
        return adapter.discardRecording(removePublished);
    }
    static float render(JuceAudioDeviceAdapter& adapter) {
        // Never initialise AudioDeviceManager: exclusive, synchronous test consumer.
        adapter.realtimeEngine_.deviceInitialising();
        adapter.realtimeEngine_.deviceConsumerStarted();
        if (!adapter.tryRequestPlay().accepted) std::abort();
        std::array<float,1024> left{}, right{};
        std::array<float*,2> channels{left.data(),right.data()};
        realtimeAllocations.store(0, std::memory_order_relaxed);
        countRealtimeAllocations.store(true, std::memory_order_relaxed);
        adapter.realtimeEngine_.processBlock({channels.data(),2,left.size()},timeline::SampleRate{48000});
        countRealtimeAllocations.store(false, std::memory_order_relaxed);
        if (realtimeAllocations.load(std::memory_order_relaxed) != 0) std::abort();
        return left[10];
    }
};
}
namespace {
using namespace vitadaw;
void check(bool condition,const char* text){if(!condition){std::cerr<<text<<'\n';std::exit(1);}}
void wav(const juce::File& file,float sample,double rate){
    juce::AudioBuffer<float> buffer(1,480);for(int i=0;i<480;++i)buffer.setSample(0,i,sample);
    juce::WavAudioFormat format;
    std::unique_ptr<juce::OutputStream> stream=file.createOutputStream();
    auto writer=format.createWriterFor(stream,juce::AudioFormatWriterOptions{}.withSampleRate(rate).withNumChannels(1).withBitsPerSample(16));
    check(writer!=nullptr&&writer->writeFromAudioSampleBuffer(buffer,0,480),"write real WAV");
}
}
int main(){
    using namespace vitadaw;
    const auto dir=juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("vitadaw-media", "",false);
    check(dir.createDirectory().wasOk(),"temporary directory");
    const auto a=dir.getChildFile("a.wav"),b=dir.getChildFile("b.wav");wav(a,0.25F,44100);wav(b,-0.5F,48000);
    const auto text=dir.getChildFile("not-audio.txt");
    const auto corrupt=dir.getChildFile("corrupt.wav");
    check(text.replaceWithText("not audio")&&corrupt.replaceWithText("not a wav"),
          "invalid media fixtures");
    platform::juce_adapter::JuceAudioDeviceAdapter engine;
    application::DawApplication app{engine,timeline::SampleRate{48000}};
    auto ok=[&](commands::Command c){auto r=app.handle(c);if(r.status!=commands::CommandStatus::accepted)std::cerr<<r.message<<" "<<persistence::codeName(r.persistence.code)<<'\n';check(r.status==commands::CommandStatus::accepted,"command accepted");};
    const auto path=[&](const char* name){return std::filesystem::path{dir.getChildFile(name).getFullPathName().toStdString()};};
    check(app.project().tracks().empty(),"native New Project starts with zero tracks");
    const auto cleanToken=app.history().currentStateToken();
    const auto missing=app.handle(commands::ImportAudioFile{path("missing.wav"),{0}});
    const auto unsupported=app.handle(commands::ImportAudioFile{path("not-audio.txt"),{0}});
    const auto decodeFailure=app.handle(commands::ImportAudioFile{path("corrupt.wav"),{0}});
    check(missing.error==commands::CommandError::fileNotFound&&
          unsupported.error==commands::CommandError::unsupportedFormat&&
          decodeFailure.error==commands::CommandError::decodeFailed&&
          app.history().currentStateToken()==cleanToken&&
          app.project().tracks().empty()&&app.project().sources().empty(),
          "real file access, format and decode failures are typed and transactional");
    ok(commands::ImportAudioFile{path("a.wav"),{0}});
    const auto waveformA = app.waveformCache().find({1});
    check(app.project().tracks().size()==1&&app.project().sources().size()==1&&
          app.project().tracks()[0].clips.size()==1&&
          app.timelineSnapshot().tracks[0].clips.size()==1&&
          app.project().duration().value==523&&
          waveformA&&waveformA->levels[0].channels[0][0].maximum>0.2F,
          "actual WAV first import creates coherent source waveform and timeline duration");
    for(int i=1;i<10;++i)ok(commands::AddClip{{1},{1},{i*10},{200},{0}});
    check(app.waveformCache().size()==1&&app.waveformCache().find({1})==waveformA,
          "ten clips retain one source-level waveform identity");
    ok(commands::SaveProjectAs{path("a.vitadaw")});
    check(platform::juce_adapter::PersistenceIntegrationAccess::render(engine)>0,"A actual JUCE decoded render");app.synchroniseTransport();
    auto decoded=engine.prepareWav(path("b.wav"));check(decoded.success(),"B WAV decoded");
    project::ProjectState model{timeline::SampleRate{48000}};auto track=model.addAudioTrack("B");
    check(model.importAudioToTrack(track,decoded.prepared->media,{480},timeline::SampleRate{48000},media::AudioChannelLayout::mono).source.value==1,"B SourceId one");
    auto serialized=persistence::serializeProject(model,path("b.vitadaw"));check(serialized.result.success(),"B serialize");
    check(platform::files::nativeProjectFileIO().replace(path("b.vitadaw"),serialized.bytes).success(),"B save");
    ok(commands::LoadProject{path("b.vitadaw")});
    const auto waveformB=app.waveformCache().find({1});
    check(waveformB&&waveformB!=waveformA&&
          waveformB->levels[0].channels[0][0].minimum<-0.4F,
          "successful load replaces the session cache despite SourceId collision");
    check(platform::juce_adapter::PersistenceIntegrationAccess::render(engine)<0,"B collision renders B via actual adapter and processBlock");app.synchroniseTransport();
    const auto fingerprint=*app.project().sources()[0].media.fingerprint;
    check(a.copyFileTo(b),"replace media bytes");
    check(engine.prepareVerifiedWav(path("b.wav"),fingerprint,0).result.code==persistence::PersistenceCode::mediaChanged,"changed bytes rejected before decode");
    const auto token=app.history().currentStateToken();
    check(app.handle(commands::LoadProject{path("b.vitadaw")}).persistence.code==persistence::PersistenceCode::mediaChanged,"load rollback changed WAV");
    check(app.history().currentStateToken()==token&&app.waveformCache().find({1})==waveformB&&
          platform::juce_adapter::PersistenceIntegrationAccess::render(engine)<0,
          "old PCM and waveform survive failed load");
    app.synchroniseTransport();ok(commands::LoadProject{path("a.vitadaw")});
    check(app.project().tracks()[0].clips.size()==10&&app.project().sources().size()==1,"shared Source survives actual JUCE load");
    check(app.waveformCache().size()==1&&app.waveformCache().find({1})&&
          app.waveformCache().find({1})->levels[0].channels[0][0].maximum>0.2F,
          "waveform rebuilds from verified media on project reopen");
    check(engine.preparedAudioBytes()==480*sizeof(float),"one decoded PCM for ten clips");
    const auto externalTemporary = path(".Recording 000001.1.part.wav");
    const auto ownedTemporary = path(".Recording 000001.2.part.wav");
    const auto abandonedTemporary = path(".Recording 000001.0.part.wav");
    const auto competingFinal = path("Recording 000001.wav");
    check(juce::File{juce::String(externalTemporary.wstring().c_str())}
              .replaceWithText("external temporary"),
          "create externally owned temporary-like fixture");
    std::error_code ownershipError;
    check(platform::juce_adapter::PersistenceIntegrationAccess::closesExclusiveOnAbandon(
              abandonedTemporary, ownershipError),
          "exclusive temporary RAII closes its descriptor before stream ownership transfer");
    check(!platform::juce_adapter::PersistenceIntegrationAccess::createExclusiveTemporary(
              externalTemporary, ownershipError).has_value() &&
              juce::File{juce::String(externalTemporary.wstring().c_str())}
                  .loadFileAsString() == "external temporary",
          "exclusive temporary creation never deletes a pre-existing temporary-like file");
    ownershipError.clear();
    const auto ownedTemporaryIdentity =
        platform::juce_adapter::PersistenceIntegrationAccess::createExclusiveTemporary(
            ownedTemporary, ownershipError);
    check(ownedTemporaryIdentity &&
              !platform::juce_adapter::PersistenceIntegrationAccess::discard(
                  engine, ownedTemporary, ownedTemporaryIdentity, {}, std::nullopt, false) &&
              juce::File{juce::String(ownedTemporary.wstring().c_str())}.existsAsFile() &&
              juce::File{juce::String(externalTemporary.wstring().c_str())}.existsAsFile(),
          "temporary cleanup retains an owned orphan rather than deleting by pathname");
    const auto publishedTemporary = path(".Recording 000001.3.part.wav");
    const auto replacedTemporary = path(".Recording 000001.4.part.wav");
    const auto ownedPublishedTemporary =
        platform::juce_adapter::PersistenceIntegrationAccess::createExclusiveTemporary(
            publishedTemporary, ownershipError);
    check(ownedPublishedTemporary &&
              juce::File{juce::String(competingFinal.wstring().c_str())}
                  .replaceWithText("external final"),
          "create publication-collision fixtures");
    std::filesystem::create_hard_link(publishedTemporary, competingFinal, ownershipError);
    check(ownershipError &&
              juce::File{juce::String(competingFinal.wstring().c_str())}
                  .loadFileAsString() == "external final",
          "no-replace publication preserves a competing final file");
    check(!platform::juce_adapter::PersistenceIntegrationAccess::discard(
              engine, publishedTemporary, ownedPublishedTemporary, competingFinal, std::nullopt, true) &&
              juce::File{juce::String(competingFinal.wstring().c_str())}
                  .loadFileAsString() == "external final",
          "failed publication cleanup never deletes an unowned competing final");

    const auto ownedFinalTemporary = path(".Recording 000002.3.part.wav");
    const auto ownedFinalIdentity =
        platform::juce_adapter::PersistenceIntegrationAccess::createExclusiveTemporary(
            ownedFinalTemporary, ownershipError);
    const auto ownedFinal = path("Recording 000002.wav");
    ownershipError.clear();
    std::filesystem::create_hard_link(ownedFinalTemporary, ownedFinal, ownershipError);
    check(ownedFinalIdentity && !ownershipError &&
              !platform::juce_adapter::PersistenceIntegrationAccess::discard(
                  engine, ownedFinalTemporary, ownedFinalIdentity, ownedFinal,
                  ownedFinalIdentity, false) &&
              juce::File{juce::String(ownedFinal.wstring().c_str())}.existsAsFile(),
          "owned final survives retained temporary cleanup");
    check(!platform::juce_adapter::PersistenceIntegrationAccess::discard(
              engine, {}, std::nullopt, ownedFinal, ownedFinalIdentity, true) &&
              juce::File{juce::String(ownedFinal.wstring().c_str())}.existsAsFile(),
          "rollback retains a published final as an explicit safe orphan");

    const auto replacementTemporary = path(".Recording 000003.3.part.wav");
    const auto replacementIdentity =
        platform::juce_adapter::PersistenceIntegrationAccess::createExclusiveTemporary(
            replacementTemporary, ownershipError);
    const auto replaceableFinal = path("Recording 000003.wav");
    ownershipError.clear();
    std::filesystem::create_hard_link(replacementTemporary, replaceableFinal, ownershipError);
    check(replacementIdentity && !ownershipError && std::filesystem::remove(replaceableFinal, ownershipError) &&
              juce::File{juce::String(replaceableFinal.wstring().c_str())}
                  .replaceWithText("replacement final") &&
              !platform::juce_adapter::PersistenceIntegrationAccess::discard(
                  engine, replacementTemporary, replacementIdentity, replaceableFinal,
                  replacementIdentity, true) &&
              juce::File{juce::String(replaceableFinal.wstring().c_str())}
                  .loadFileAsString() == "replacement final",
          "cleanup rejects a final pathname replaced after publication");

    const auto replacedTemporaryIdentity =
        platform::juce_adapter::PersistenceIntegrationAccess::createExclusiveTemporary(
            replacedTemporary, ownershipError);
    check(replacedTemporaryIdentity && std::filesystem::remove(replacedTemporary, ownershipError) &&
              juce::File{juce::String(replacedTemporary.wstring().c_str())}
                  .replaceWithText("replacement temporary") &&
              !platform::juce_adapter::PersistenceIntegrationAccess::discard(
                  engine, replacedTemporary, replacedTemporaryIdentity, {}, std::nullopt, false) &&
              juce::File{juce::String(replacedTemporary.wstring().c_str())}
                  .loadFileAsString() == "replacement temporary",
          "cleanup never deletes a temporary pathname replaced after exclusive creation");
    engine.shutdown();check(dir.deleteRecursively(),"remove own temporary files");
    std::cout<<"JUCE media persistence integration passed without audio hardware\n";
}
