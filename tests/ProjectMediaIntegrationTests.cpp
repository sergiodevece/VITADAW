#include "vitadaw/platform/juce/JuceAudioDeviceAdapter.h"
#include "vitadaw/application/DawApplication.h"
#include "vitadaw/persistence/ProjectPersistence.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include <array>
#include <iostream>

namespace vitadaw::platform::juce_adapter {
class PersistenceIntegrationAccess {
public:
    static float render(JuceAudioDeviceAdapter& adapter) {
        // Never initialise AudioDeviceManager: exclusive, synchronous test consumer.
        adapter.realtimeEngine_.deviceInitialising();
        adapter.realtimeEngine_.deviceConsumerStarted();
        if (!adapter.tryRequestPlay().accepted) std::abort();
        std::array<float,1024> left{}, right{};
        std::array<float*,2> channels{left.data(),right.data()};
        adapter.realtimeEngine_.processBlock({channels.data(),2,left.size()},timeline::SampleRate{48000});
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
    platform::juce_adapter::JuceAudioDeviceAdapter engine;
    application::DawApplication app{engine,timeline::SampleRate{48000}};
    auto ok=[&](commands::Command c){auto r=app.handle(c);if(r.status!=commands::CommandStatus::accepted)std::cerr<<r.message<<" "<<persistence::codeName(r.persistence.code)<<'\n';check(r.status==commands::CommandStatus::accepted,"command accepted");};
    const auto path=[&](const char* name){return std::filesystem::path{dir.getChildFile(name).getFullPathName().toStdString()};};
    ok(commands::AddAudioTrack{"A"});ok(commands::ImportAudioToTrack{path("a.wav"),{1},{0}});
    for(int i=1;i<10;++i)ok(commands::AddClip{{1},{1},{i*10},{200},{0}});
    ok(commands::SaveProjectAs{path("a.vitadaw")});
    check(platform::juce_adapter::PersistenceIntegrationAccess::render(engine)>0,"A actual JUCE decoded render");app.synchroniseTransport();
    auto decoded=engine.prepareWav(path("b.wav"));check(decoded.success(),"B WAV decoded");
    project::ProjectState model{timeline::SampleRate{48000}};auto track=model.addAudioTrack("B");
    check(model.importAudioToTrack(track,decoded.prepared->media,{480},timeline::SampleRate{48000},media::AudioChannelLayout::mono).source.value==1,"B SourceId one");
    auto serialized=persistence::serializeProject(model,path("b.vitadaw"));check(serialized.result.success(),"B serialize");
    check(platform::files::nativeProjectFileIO().replace(path("b.vitadaw"),serialized.bytes).success(),"B save");
    ok(commands::LoadProject{path("b.vitadaw")});
    check(platform::juce_adapter::PersistenceIntegrationAccess::render(engine)<0,"B collision renders B via actual adapter and processBlock");app.synchroniseTransport();
    const auto fingerprint=*app.project().sources()[0].media.fingerprint;
    check(a.copyFileTo(b),"replace media bytes");
    check(engine.prepareVerifiedWav(path("b.wav"),fingerprint,0).result.code==persistence::PersistenceCode::mediaChanged,"changed bytes rejected before decode");
    const auto token=app.history().currentStateToken();
    check(app.handle(commands::LoadProject{path("b.vitadaw")}).persistence.code==persistence::PersistenceCode::mediaChanged,"load rollback changed WAV");
    check(app.history().currentStateToken()==token&&platform::juce_adapter::PersistenceIntegrationAccess::render(engine)<0,"old PCM survives failed load");
    app.synchroniseTransport();ok(commands::LoadProject{path("a.vitadaw")});
    check(app.project().tracks()[0].clips.size()==10&&app.project().sources().size()==1,"shared Source survives actual JUCE load");
    check(engine.preparedAudioBytes()==480*sizeof(float),"one decoded PCM for ten clips");
    engine.shutdown();check(dir.deleteRecursively(),"remove own temporary files");
    std::cout<<"JUCE media persistence integration passed without audio hardware\n";
}
