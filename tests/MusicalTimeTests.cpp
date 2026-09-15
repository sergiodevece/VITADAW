#include "vitadaw/musical/MusicalTime.h"
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <random>

using namespace vitadaw;
using namespace musical;
namespace {
void check(bool b, const char* message) { if (!b) { std::cerr << message << '\n'; std::exit(1); } }
void near(double a, long double b, long double tolerance=1e-7L) {
    if (std::abs(static_cast<long double>(a)-b)>tolerance) { std::cerr<<a<<" != "<<static_cast<double>(b)<<'\n'; std::exit(1); }
}
auto prepare(const MusicalTimeMap& m, double rate=48000) {
    auto p=PreparedMusicalTimeMap::compile(m,timeline::SampleRate{rate}); check(bool(p),"compile"); return std::move(p.value);
}
void golden() {
    MusicalTimeMap m; auto p=prepare(m);
    check(p->projectFrameAt(MusicalTickPosition{ppq}).value.value==24000,"120 quarter");
    check(p->projectFrameAt(MusicalPosition{{1},{0},{0}}).value.value==96000,"120 bar");
    m.tempo.events.push_back({{2},{16*ppq},{60}}); m.tempo.events.push_back({{3},{32*ppq},{180}}); m.tempo.nextId={4};
    m.signatures.events.push_back({{2},{4},{3,4}}); m.signatures.events.push_back({{3},{8},{7,8}}); m.signatures.nextId={4};
    p=prepare(m);
    check(p->projectFrameAt(MusicalTickPosition{16*ppq}).value.value==384000,"four bars120");
    check(p->projectFrameAt(MusicalTickPosition{32*ppq}).value.value==1152000,"next four bars60");
    check(p->projectFrameAt(MusicalTickPosition{33*ppq}).value.value==1168000,"quarter180");
    check(p->tickAt({{4},{0},{0}}).value.value==245760,"bar5 tick");
    check(p->tickAt({{8},{0},{0}}).value.value==430080,"bar9 tick");
    check(p->tickAt({{9},{0},{0}}).value.value==483840,"bar10 tick");
    for (auto [frame,bpm]: {std::pair{383999,120.0}, {384000,60.0}, {384001,60.0}, {1151999,60.0}, {1152000,180.0}})
        near(p->tempoAt(timeline::ProjectFramePosition{frame}).value.value,bpm);
    for (const auto bar : {4,8}) {
        const auto frame=p->projectFrameAt(MusicalPosition{{bar},{0},{0}}).value;
        check(p->musicalPositionAt(frame).value==MusicalPosition{{bar},{0},{0}},"signature exact boundary");
        check(p->musicalPositionAt(timeline::ProjectFramePosition{frame.value-1}).value.bar.value==bar-1,"signature before");
        check(p->musicalPositionAt(timeline::ProjectFramePosition{frame.value+1}).value.bar.value==bar,"signature after");
    }
    check(quantizeFrame({12.5},Rounding::nearest).value.value==13,"ties upward");
    check(quantizeFrame({12.1},Rounding::ceil).value.value==13,"exclusive ceil");
    check(quantizeFrame({12.9},Rounding::floor).value.value==12,"floor");
    check(!p->tickAt({{8},{7},{0}}),"invalid beat");
    m.tempo.events={{{1},{0},{120}},{{2},{777},{123.456}}};m.tempo.nextId={3};p=prepare(m);
    const long double oracle=(777.L/ppq*0.5L+(2.L-777.L/ppq)*60.L/123.456L)*48000.L;
    near(p->preciseProjectFrameAt({2}).value.value,oracle);
}
void exactDspPreparation() {
    MusicalTimeMap map;
    map.tempo.events[0].bpm = {123.0};
    auto prepared = prepare(map, 48000.0);
    const auto beat = prepared->exactProjectFrameAtTick({ppq});
    check(bool(beat), "123 BPM exact beat prepares");
    check(audio::exact::comparePositions(
              beat.value, {23414, {{26,0},{41,0},false}}) == 0,
          "48000/123 beat is exactly 960000/41 frames");
    check(audio::exact::comparePositions(
              {23414, {{25,0},{41,0},false}}, beat.value) < 0,
          "rational position immediately before beat remains before");
    check(audio::exact::comparePositions(
              {23414, {{27,0},{41,0},false}}, beat.value) > 0,
          "rational position immediately after beat remains after");

    map.tempo.events.push_back({{2},{ppq},{std::nextafter(123.0, 124.0)}});
    map.tempo.events.push_back({{3},{3*ppq},{123.5}});
    map.tempo.nextId = {4};
    prepared = prepare(map, 48000.0);
    const auto firstAnchor = prepared->exactProjectFrameAtTick({ppq});
    const auto secondAnchor = prepared->exactProjectFrameAtTick({3*ppq});
    check(firstAnchor && secondAnchor &&
          audio::exact::comparePositions(firstAnchor.value, beat.value) == 0,
          "later tempo events do not perturb the exact prior anchor");
    check(audio::exact::comparePositions(secondAnchor.value, firstAnchor.value) > 0,
          "neighboring-binary64 and fractional BPM segments remain monotonic");
}
void numeric() {
    std::mt19937_64 rng{5242};
    for (double rate:{44100.,48000.,96000.}) {
        MusicalTimeMap m; m.tempo.events.clear();m.signatures.events.clear();
        for (std::int64_t n=0;n<4096;++n) {
            m.tempo.events.push_back({{static_cast<std::uint64_t>(n+1)},{n*19723},{20.0+double(n%381)}});
            m.signatures.events.push_back({{static_cast<std::uint64_t>(n+1)},{n*2},{unsigned(n%32+1),1u<<unsigned(n%7)}});
        }
        m.tempo.nextId={4097};m.signatures.nextId={4097};auto p=prepare(m,rate);
        std::array<GridLine,64> gridLines;
        for (int n=0;n<100;++n) {
            const double start=double(rng()%1000000000ULL);
            auto result=p->enumerateGridLines({start},{start+10000000},{GridKind::bars,1},gridLines);
            check(result.error==Error::none,"many-event grid");
            for(std::size_t i=0;i<result.count;++i) {
                const auto tick=p->tickAt(gridLines[i].position);check(bool(tick),"grid position valid");
                near(gridLines[i].frame.value,p->preciseProjectFrameAt({double(tick.value.value)/ppq}).value.value);
            }
        }
        std::array<long double,4096> prefix{};
        for (std::size_t n=1;n<prefix.size();++n) prefix[n]=prefix[n-1]+19723.L/ppq*60.L/m.tempo.events[n-1].bpm.value;
        for (int n=0;n<10000;++n) {
            const auto tick=static_cast<std::int64_t>(rng()%1000000000ULL);
            const auto index=std::min<std::size_t>(tick/19723,4095);
            const long double q=static_cast<long double>(tick)/ppq;
            const auto oracle=(prefix[index]+(q-static_cast<long double>(index*19723)/ppq)*60.L/m.tempo.events[index].bpm.value)*rate;
            auto f=p->preciseProjectFrameAt({double(q)});check(bool(f),"long frame");
            if(std::abs(static_cast<long double>(f.value.value)-oracle)>1e-5L)std::cerr<<"tick="<<tick<<" index="<<index<<" rate="<<rate<<'\n';
            near(f.value.value,oracle,1e-5L);
            const auto inverse=p->quarterNotePositionAt(f.value);check(bool(inverse),"inverse");
            check(std::llround(inverse.value.value*ppq)==tick,"tick precise frame tick");
            const auto integer=timeline::ProjectFramePosition{static_cast<std::int64_t>(rng()%100000000000ULL)};
            const auto qi=p->quarterNotePositionAt(integer);check(bool(qi),"days inverse");
            const auto back=p->preciseProjectFrameAt(qi.value);
            check(back && quantizeFrame(back.value,Rounding::nearest).value==integer,"integer frame exact roundtrip over days");
        }
        // Strict floor around representable boundaries, including fractional BPM.
        MusicalTimeMap fractional; fractional.tempo.events[0].bpm={123.456};auto f=prepare(fractional,rate);
        for (std::int64_t tick=1;tick<100000;tick+=79) {
            auto at=f->preciseProjectFrameAt({double(tick)/ppq}).value;
            const auto before=timeline::PreciseProjectFramePosition{std::nextafter(at.value,0.0)};
            check(f->tickAt(f->musicalPositionAt(at).value).value.value==tick,"display exact boundary");
            check(f->tickAt(f->musicalPositionAt(before).value).value.value==tick-1,"display strictly before boundary");
        }
    }
}
void grid() {
    MusicalTimeMap m;m.tempo.events.push_back({{2},{777},{60}});m.tempo.nextId={3};auto p=prepare(m);
    std::array<GridLine,5> out;
    auto r=p->enumerateBeats({2000},{400000},out);check(r.count==5&&r.hasMore,"bounded grid");
    check(out[0].position.beat.value==1,"mid-beat start skips prior grid");
    auto second=p->enumerateBeats(r.nextStart,{400000},out);check(second.count>0,"continuation");
    check(out[0].frame==r.nextStart,"continuation includes omitted line");
    std::array<GridLine,64> full;
    r=p->enumerateGridLines({0},{100000},{GridKind::subdivisions,4},full);
    check(r.count>4&&!r.hasMore,"subdivision grid");
    for(std::size_t i=1;i<r.count;++i)check(full[i].frame.value>full[i-1].frame.value,"grid monotonic");
    check(p->enumerateBeats({0},{0},out).count==0,"empty range");
    check(p->enumerateBeats({0},{1},{}).hasMore,"zero capacity");
    m.tempo.events.clear();
    for(std::int64_t i=0;i<4096;++i)m.tempo.events.push_back({{static_cast<std::uint64_t>(i+1)},{i},{20.0+double(i%381)}});
    m.tempo.nextId={4097};p=prepare(m);
    r=p->enumerateGridLines({0},{1000000},{GridKind::bars,1},full);
    check(r.count>1,"4096 changes before second grid line");
    near(full[1].frame.value,p->preciseProjectFrameAt({4}).value.value);
}
void validation() {
    auto invalid=[](MusicalTimeMap m){check(!PreparedMusicalTimeMap::compile(m,timeline::SampleRate{48000}),"invalid map rejected");};
    MusicalTimeMap m;
    for(double bpm:{0.,19.,401.,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()}){auto x=m;x.tempo.events[0].bpm={bpm};invalid(x);}
    for(unsigned d:{0u,3u,7u,128u}){auto x=m;x.signatures.events[0].signature.denominator=d;invalid(x);}
    auto x=m;x.tempo.events.clear();invalid(x);x=m;x.signatures.events.clear();invalid(x);
    x=m;x.resolution=960;invalid(x);x=m;x.tempo.events[0].id={0};invalid(x);
    x=m;x.tempo.events[0].curveToNext=static_cast<TempoCurve>(1);invalid(x);
    x=m;x.tempo.events.push_back({{2},{0},{60}});x.tempo.nextId={3};invalid(x);
    x=m;x.signatures.events.push_back({{2},{maximumCoordinate},{4,4}});x.signatures.nextId={3};invalid(x);
    x=m;x.tempo.events.resize(4097);invalid(x);
    check(!PreparedMusicalTimeMap::compile(m,timeline::SampleRate{0}),"invalid rate");
    auto p=prepare(m);check(!p->preciseProjectFrameAt({-1}),"negative query");
    check(!p->quarterNotePositionAt(timeline::PreciseProjectFramePosition{double(maximumCoordinate)*2}),"overflow query");
}
}
int main(){golden();exactDspPreparation();numeric();grid();validation();std::cout<<"Musical time tests passed\n";}
