#include "vitadaw/musical/MusicalTime.h"

#include <cstdlib>
#include <bit>
#include <iomanip>
#include <iostream>
#include <string>

namespace {
void print128(vitadaw::audio::exact::UInt128 value) {
    std::cout << std::hex << value.hi << ' ' << value.lo << ' ';
}
}

int main() {
    using namespace vitadaw;
    std::string rateText;
    std::size_t eventCount{}, signatureCount{}, queryCount{};
    while (std::cin >> rateText >> eventCount >> signatureCount >> queryCount) {
        musical::MusicalTimeMap map;
        map.tempo.events.clear();
        for (std::size_t index=0; index<eventCount; ++index) {
            std::int64_t tick;
            std::string bpmText;
            std::cin >> tick >> bpmText;
            map.tempo.events.push_back({{index+1},{tick},
                {std::strtod(bpmText.c_str(),nullptr)}});
        }
        map.tempo.nextId={eventCount+1};
        map.signatures.events.clear();
        for (std::size_t index=0; index<signatureCount; ++index) {
            std::int64_t bar;
            unsigned numerator, denominator;
            std::cin >> bar >> numerator >> denominator;
            map.signatures.events.push_back({{index+1},{bar},
                {numerator,denominator}});
        }
        map.signatures.nextId={signatureCount+1};
        auto prepared=musical::PreparedMusicalTimeMap::compile(
            map,timeline::SampleRate{std::strtod(rateText.c_str(),nullptr)});
        for (std::size_t index=0; index<queryCount; ++index) {
            std::int64_t tick;
            std::cin >> tick;
            if (!prepared) {
                std::cout << "0\n";
                continue;
            }
            const auto position=prepared.value->exactProjectFrameAtTick({tick});
            if (!position) {
                std::cout << "0\n";
                continue;
            }
            const auto floor=audio::exact::floorPosition(position.value);
            std::cout << "1 " << std::dec << floor.frame << ' ';
            print128(floor.remainder);
            print128(floor.denominator);
            const auto inverse=prepared.value->absoluteTickAt(position.value);
            const auto roundedFloor=prepared.value->projectFrameAt(
                musical::MusicalTickPosition{tick},musical::Rounding::floor);
            const auto roundedCeil=prepared.value->projectFrameAt(
                musical::MusicalTickPosition{tick},musical::Rounding::ceil);
            const auto roundedNearest=prepared.value->projectFrameAt(
                musical::MusicalTickPosition{tick},musical::Rounding::nearest);
            const auto musicalPosition=prepared.value->musicalPositionAt(position.value);
            const auto tempo=prepared.value->tempoAt(position.value);
            const auto signature=prepared.value->timeSignatureAt(position.value);
            if (!inverse || !roundedFloor || !roundedCeil || !roundedNearest ||
                !musicalPosition || !tempo || !signature) {
                std::cout << "0\n";
                continue;
            }
            std::cout << std::dec << inverse.value.value << ' '
                      << roundedFloor.value.value << ' '
                      << roundedCeil.value.value << ' '
                      << roundedNearest.value.value << ' '
                      << musicalPosition.value.bar.value << ' '
                      << musicalPosition.value.beat.value << ' '
                      << musicalPosition.value.tick.value << ' '
                      << std::hex << std::bit_cast<std::uint64_t>(tempo.value.value)
                      << std::dec << ' ' << signature.value.numerator << ' '
                      << signature.value.denominator;
            std::cout << '\n';
        }
    }
}
