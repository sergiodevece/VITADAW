#include "vitadaw/musical/MusicalTime.h"

#include <cstdlib>
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
    std::size_t eventCount{}, queryCount{};
    while (std::cin >> rateText >> eventCount >> queryCount) {
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
            std::cout << '\n';
        }
    }
}
