#include <iostream>
#include <string>

#include "v151/ExternalSequence.h"

namespace {

bool loadAndVerifyTrid(const std::string& path)
{
    ExternalSequence seq;
    if (!seq.load(path)) {
        std::cerr << "Failed to load " << path << "\n";
        return false;
    }

    SeqBlock* block = seq.GetBlock(0);
    if (!block) {
        std::cerr << "Block 0 is null\n";
        return false;
    }

    if (!seq.decodeBlock(block)) {
        std::cerr << "Failed to decode block 0\n";
        return false;
    }

    const auto& labelSets = block->GetLabelSetEvents();
    if (labelSets.empty()) {
        std::cerr << "Block 0 has no LABELSET events\n";
        return false;
    }

    const int labelId = labelSets.front().numVal.first;
    const int value = labelSets.front().numVal.second;
    const std::string name = seq.GetUnknownLabelName(labelId);
    const std::string counterName = seq.getCounterIdAsString(labelId);

    if (labelId < 1000 || value != 3 || name != "TRID" || counterName != "TRID") {
        std::cerr << "Unexpected TRID decode: id=" << labelId
                  << " value=" << value
                  << " unknownName=" << name
                  << " counterName=" << counterName << "\n";
        return false;
    }

    delete block;
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        std::cerr << "Usage: TridReopenStateTest <seq-file>\n";
        return 2;
    }

    const std::string path = argv[1];
    if (!loadAndVerifyTrid(path))
        return 1;

    // The second fresh parser instance is the regression: TRID must not depend on
    // a function-local static map populated by the first load.
    if (!loadAndVerifyTrid(path))
        return 1;

    return 0;
}
