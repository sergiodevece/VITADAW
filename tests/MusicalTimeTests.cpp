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
    auto p=PreparedMusicalTimeMap::compile(m,timeline::SampleRate{rate});
    if (!p) std::cerr << "compile: " << errorName(p.error) << '\n';
    check(bool(p),"compile"); return std::move(p.value);
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
audio::exact::Position oneRationalUnitBefore(audio::exact::Position position) {
    auto floor=audio::exact::floorPosition(position);
    if (!audio::exact::zero(audio::exact::wide(floor.remainder))) {
        audio::exact::UInt256 reduced;
        audio::exact::subtract(audio::exact::wide(floor.remainder),
                               audio::exact::wide(1), reduced);
        return {floor.frame,{audio::exact::low128(reduced),floor.denominator,false}};
    }
    audio::exact::UInt256 reduced;
    audio::exact::subtract(audio::exact::wide(floor.denominator),
                           audio::exact::wide(1), reduced);
    return {floor.frame-1,{audio::exact::low128(reduced),floor.denominator,false}};
}
audio::exact::Position oneRationalUnitAfter(audio::exact::Position position) {
    auto floor=audio::exact::floorPosition(position);
    audio::exact::UInt256 increased;
    audio::exact::add(audio::exact::wide(floor.remainder),
                      audio::exact::wide(1), increased);
    if (audio::exact::compare(increased,
                              audio::exact::wide(floor.denominator)) < 0)
        return {floor.frame,{audio::exact::low128(increased),floor.denominator,false}};
    return {floor.frame+1,{}};
}
void exactBidirectionalQueries() {
    for (const auto rate:{44100.0,48000.0,96000.0}) {
        for (const auto bpm:{120.0,123.0,std::nextafter(123.0,124.0),
                            std::nextafter(123.0,122.0)}) {
            MusicalTimeMap map; map.tempo.events[0].bpm={bpm};
            auto prepared=prepare(map,rate);
            for (const auto tick:{std::int64_t{0},std::int64_t{1},
                                 std::int64_t{ppq-1},std::int64_t{ppq},
                                 std::int64_t{7*ppq},maximumCoordinate-1,
                                 maximumCoordinate}) {
                const auto position=prepared->exactProjectFrameAtTick({tick});
                check(bool(position),"forward exact query");
                const auto inverse=prepared->absoluteTickAt(position.value);
                check(inverse&&inverse.value.value==tick,"exact forward/inverse roundtrip");
            }
            for (const auto frame:{std::int64_t{0},std::int64_t{64},
                                  std::int64_t{511},std::int64_t{1000000}}) {
                const audio::exact::Position position{frame,{}};
                const auto tick=prepared->absoluteTickAt(position);
                check(bool(tick),"integer project position inverse");
                const auto lower=prepared->exactProjectFrameAtTick(tick.value);
                check(lower&&audio::exact::comparePositions(lower.value,position)<=0,
                      "inverse lower boundary does not exceed position");
                if(tick.value.value<maximumCoordinate){
                    const auto upper=prepared->exactProjectFrameAtTick(
                        {tick.value.value+1});
                    check(upper&&audio::exact::comparePositions(upper.value,position)>0,
                          "inverse returns greatest tick at or below position");
                }
            }
        }
    }

    MusicalTimeMap segmented;
    segmented.tempo.events={{{1},{0},{123.0}},
                            {{2},{ppq},{std::nextafter(123.0,124.0)}},
                            {{3},{3*ppq},{120.0}}};
    segmented.tempo.nextId={4};
    segmented.signatures.events.push_back({{2},{1},{7,8}});
    segmented.signatures.nextId={3};
    auto prepared=prepare(segmented);
    const auto tempoAnchor=prepared->exactProjectFrameAtTick({ppq});
    check(bool(tempoAnchor),"tempo anchor");
    const auto before=oneRationalUnitBefore(tempoAnchor.value);
    const auto after=oneRationalUnitAfter(tempoAnchor.value);
    check(prepared->absoluteTickAt(before).value.value==ppq-1,
          "inverse before next segment anchor stays in prior segment");
    check(prepared->absoluteTickAt(tempoAnchor.value).value.value==ppq,
          "inverse exact next segment anchor selects new segment");
    check(prepared->absoluteTickAt(after).value.value==ppq,
          "inverse after next segment anchor remains in new segment");
    check(prepared->tempoAt(before).value.value==123.0,
          "tempo before segment anchor");
    check(prepared->tempoAt(tempoAnchor.value).value.value==
              std::nextafter(123.0,124.0),
          "tempo at segment anchor belongs to new segment");

    const auto signatureTick=prepared->tickAt({{1},{0},{0}});
    check(bool(signatureTick),"signature tick");
    const auto signatureAnchor=prepared->exactProjectFrameAtTick(signatureTick.value);
    check(bool(signatureAnchor),"signature anchor");
    check(prepared->timeSignatureAt(
              oneRationalUnitBefore(signatureAnchor.value)).value==TimeSignature{4,4},
          "signature before bar anchor");
    check(prepared->timeSignatureAt(signatureAnchor.value).value==TimeSignature{7,8},
          "signature at bar anchor");

    MusicalTimeMap rounding; auto rounded=prepare(rounding);
    check(rounded->projectFrameAt(MusicalTickPosition{8},Rounding::floor).value.value==12,
          "exact floor");
    check(rounded->projectFrameAt(MusicalTickPosition{8},Rounding::ceil).value.value==13,
          "exact ceil");
    check(rounded->projectFrameAt(MusicalTickPosition{8},Rounding::nearest).value.value==13,
          "exact nearest tie rounds upward");
    check(rounded->projectFrameAt(MusicalTickPosition{4},Rounding::nearest).value.value==6,
          "exact nearest below half");

    MusicalTimeMap dense; dense.tempo.events[0].bpm={400};
    auto densePrepared=prepare(dense);
    check(densePrepared->projectFrameAt(MusicalTickPosition{2}).value.value==1&&
          densePrepared->projectFrameAt(MusicalTickPosition{3}).value.value==1,
          "multiple ticks may round to one project frame");
    check(densePrepared->absoluteTickAt(
              timeline::ProjectFramePosition{1}).value.value==2,
          "rounded tick to integer frame is deliberately not a reversible map");
}
void numeric() {
    std::mt19937_64 rng{5242};
    for (double rate:{44100.,48000.,96000.}) {
        MusicalTimeMap m; m.tempo.events.clear();m.signatures.events.clear();
        for (std::int64_t n=0;n<4096;++n) {
            m.tempo.events.push_back({{static_cast<std::uint64_t>(n+1)},{n*19723},
                {n % 2 == 0 ? 120.0 : 240.0}});
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
                near(gridLines[i].frame.value,
                     p->preciseProjectFrameAtTick(tick.value).value.value);
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
        // PreciseProjectFramePosition is presentation input: interpret its own
        // binary64 bits exactly rather than pretending it is an exact tick anchor.
        MusicalTimeMap fractional; fractional.tempo.events[0].bpm={123.456};auto f=prepare(fractional,rate);
        for (std::int64_t tick=1;tick<100000;tick+=79) {
            const auto at=f->exactProjectFrameAtTick({tick});
            check(at && f->absoluteTickAt(at.value).value.value==tick,
                  "exact boundary inverse");
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
    for(std::int64_t i=0;i<4096;++i)m.tempo.events.push_back({{static_cast<std::uint64_t>(i+1)},{i},
        {i % 2 == 0 ? 120.0 : 240.0}});
    m.tempo.nextId={4097};p=prepare(m);
    r=p->enumerateGridLines({0},{1000000},{GridKind::bars,1},full);
    check(r.count>1,"4096 changes before second grid line");
    const auto secondTick=p->tickAt(full[1].position);
    check(bool(secondTick),"second grid tick");
    near(full[1].frame.value,p->preciseProjectFrameAtTick(secondTick.value).value.value);
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
    check(!p->absoluteTickAt({std::numeric_limits<std::int64_t>::min(),
        {{1,0},{2,0},true}}),"unrepresentable negative exact phase rejected");

    auto invalidTempo=m; invalidTempo.tempo.events[0].bpm={19};
    check(PreparedMusicalTimeMap::compile(invalidTempo,timeline::SampleRate{48000}).error==Error::invalidTempo,
          "document error remains distinct");
    auto outside=m; outside.tempo.events.push_back({{2},{maximumCoordinate+1},{120}});
    outside.tempo.nextId={3};
    check(PreparedMusicalTimeMap::compile(outside,timeline::SampleRate{48000}).error==Error::outOfRange,
          "musical coordinate range remains distinct");
    check(PreparedMusicalTimeMap::compile(m,timeline::SampleRate{1.0e9}).error==Error::conversionOverflow,
          "mapped project-frame overflow remains distinct");

    MusicalTimeMap uncertified;
    uncertified.tempo.events={{{1},{0},{123.456789012345}},
                              {{2},{1},{234.567890123456}},
                              {{3},{2},{345.678901234567}},
                              {{4},{3},{211.111111111111}}};
    uncertified.tempo.nextId={5};
    const auto certification=PreparedMusicalTimeMap::compile(
        uncertified,timeline::SampleRate{48000});
    if (certification.error!=Error::capacityExceeded)
        std::cerr<<"certification error: "<<errorName(certification.error)<<'\n';
    check(certification.error==Error::capacityExceeded,
          "exact certification failure remains distinct");
}
}
int main(){golden();exactDspPreparation();exactBidirectionalQueries();numeric();grid();validation();std::cout<<"Musical time tests passed\n";}
