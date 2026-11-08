#include "LevelSaveConverter.h"
#include <iostream>
#include <exception>

int main(int argc, char** argv)
{
    try {
        if (argc < 2 || argc > 3) {
            std::cout << "Usage: pvz-save-repair INPUT.dat64 [NEW_OUTPUT.dat64]\n"
                         "Without output: scan only. Existing files are never overwritten.\n";
            return argc == 1 ? 0 : 2;
        }
        const auto repairs = PvzDat64::RepairPlantAttacks(
            std::filesystem::u8path(argv[1]), argc == 3 ? std::filesystem::u8path(argv[2]) : std::filesystem::path{});
        for (const auto& repair : repairs)
            std::cout << "Completed Cob Cannon attack with inactive timer: slot=" << repair.mSlot
                      << " row=" << repair.mRow + 1 << " column=" << repair.mColumn + 1 << '\n';
        std::cout << (argc == 3 ? "Repaired copy written; matches=" : "Scan only; matches=") << repairs.size() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
