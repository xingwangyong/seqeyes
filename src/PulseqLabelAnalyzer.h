#pragma once

#include "external/pulseq/v151/ExternalSequence.h"
#include <map>
#include <vector>
#include <string>
#include <iostream>

/**
 * @brief Pulseq label analyzer
 * 
 * Correctly handles both LABELSET and LABELINC modes and tracks label state changes.
 */
// Forward declaration
struct LabelOperation {
    std::string type;
    int labelId;
    int flagId;
    int value;
    bool flagValue;
};

class PulseqLabelAnalyzer {
private:
    ExternalSequence& m_seq;
    LabelStateAndBookkeeping m_labelTracker;
    std::map<int, LabelValueStorage> m_blockLabelStates; // label state after each block
    
public:
    PulseqLabelAnalyzer(ExternalSequence& seq) : m_seq(seq) {
        m_labelTracker.initBookkeeping();
        m_labelTracker.initCurrState();
        analyzeLabels();
    }
    
    void analyzeLabels() {
        // Track label state changes across blocks.
        for (int i = 0; i < m_seq.GetNumberOfBlocks(); i++) {
            SeqBlock* block = m_seq.GetBlock(i);
            
            // Apply label updates to LabelStateAndBookkeeping.
            m_labelTracker.updateLabelValues(block);
            
            // Capture the updated state.
            LabelValueStorage state = getCurrentLabelState();
            m_blockLabelStates[i] = state;
            
            delete block;
        }
    }
    
    // Get label operations in a block.
    std::vector<LabelOperation> getLabelOperations(int blockIndex) {
        std::vector<LabelOperation> operations;
        SeqBlock* block = m_seq.GetBlock(blockIndex);
        
        auto labelsets = block->GetLabelSetEvents();
        for (const auto& ls : labelsets) {
            LabelOperation op;
            op.type = "SET";
            op.labelId = ls.numVal.first;
            op.flagId = ls.flagVal.first;
            op.value = ls.numVal.second;
            op.flagValue = ls.flagVal.second;
            operations.push_back(op);
        }
        
        auto labelincs = block->GetLabelIncEvents();
        for (const auto& li : labelincs) {
            LabelOperation op;
            op.type = "INC";
            op.labelId = li.numVal.first;
            op.value = li.numVal.second;
            operations.push_back(op);
        }
        
        delete block;
        return operations;
    }
    
    // Get the label state after a block executes.
    LabelValueStorage getLabelStateAfterBlock(int blockIndex) {
        // Check whether blockIndex exists in the cached map.
        if (m_blockLabelStates.find(blockIndex) != m_blockLabelStates.end()) {
            return m_blockLabelStates[blockIndex];
        }
        
        // If the block doesn't exist, return an empty LabelValueStorage.
        LabelValueStorage emptyState;
        emptyState.num.val.resize(NUM_LABELS, 0);
        emptyState.num.bValUsed.resize(NUM_LABELS, false);
        emptyState.num.bValUpdated.resize(NUM_LABELS, false);
        return emptyState;
    }
    
    // Find blocks containing a specific label value.
    std::vector<int> findBlocksWithLabel(int labelId, int targetValue) {
        std::vector<int> result;
        
        // Iterate over all cached states and find matching blocks.
        for (const auto& [blockIndex, state] : m_blockLabelStates) {
            if (labelId >= 0 && labelId < NUM_LABELS && state.num.bValUsed[labelId] && state.num.val[labelId] == targetValue) {
                result.push_back(blockIndex);
            }
        }
        
        return result;
    }
    
    // Print label update history.
    void printLabelHistory() {
        for (int i = 0; i < m_seq.GetNumberOfBlocks(); i++) {
            auto operations = getLabelOperations(i);
            if (!operations.empty()) {
                std::cout << "Block " << i << ":\n";
                for (const auto& op : operations) {
                    if (op.type == "SET") {
                        if (op.labelId != LABEL_UNKNOWN) {
                            std::string name = m_seq.getCounterIdAsString(op.labelId);
                            std::cout << "  SET " << name << " = " << op.value << "\n";
                        }
                        if (op.flagId != FLAG_UNKNOWN) {
                            std::string name = m_seq.getFlagIdAsString(op.flagId);
                            std::cout << "  SET " << name << " = " << (op.flagValue ? "true" : "false") << "\n";
                        }
                    } else if (op.type == "INC") {
                        std::string name = m_seq.getCounterIdAsString(op.labelId);
                        std::cout << "  INC " << name << " += " << op.value << "\n";
                    }
                }
                
                // Show the updated state after this block.
                auto state = getLabelStateAfterBlock(i);
                std::cout << "  Result: ";
                for (int lbl = 0; lbl < NUM_LABELS; lbl++) {
                    if (lbl >= 0 && lbl < NUM_LABELS && state.num.bValUsed[lbl]) {
                        std::string name = m_seq.getCounterIdAsString(lbl);
                        std::cout << name << "=" << state.num.val[lbl] << " ";
                    }
                }
                std::cout << "\n\n";
            }
        }
    }
    
    // Get the value range for a specific label.
    std::pair<int, int> getLabelValueRange(int labelId) {
        int minVal = INT_MAX;
        int maxVal = INT_MIN;
        bool found = false;
        
        // Iterate over all cached states and compute the real value range.
        for (const auto& [blockIndex, state] : m_blockLabelStates) {
            if (labelId >= 0 && labelId < NUM_LABELS && state.num.bValUsed[labelId]) {
                int val = state.num.val[labelId];
                if (val < minVal) minVal = val;
                if (val > maxVal) maxVal = val;
                found = true;
            }
        }
        
        // If no values were found, return a default range.
        if (!found) {
            return {0, 0};
        }
        
        return {minVal, maxVal};
    }
    
    // Get the value sequence (blockIndex, value) for a label.
    std::vector<std::pair<int, int>> getLabelValueSequence(int labelId) {
        std::vector<std::pair<int, int>> sequence;
        
        // Iterate over all cached states and record the value sequence.
        for (const auto& [blockIndex, state] : m_blockLabelStates) {
            if (labelId >= 0 && labelId < NUM_LABELS && state.num.bValUsed[labelId]) {
                sequence.push_back({blockIndex, state.num.val[labelId]});
            }
        }
        
        return sequence;
    }
    
private:
    LabelValueStorage getCurrentLabelState() {
        // Get the current state from LabelStateAndBookkeeping.
        // We create a new LabelValueStorage and fill it via the public API.
        LabelValueStorage state;
        state.num.val.resize(NUM_LABELS, 0);
        state.num.bValUsed.resize(NUM_LABELS, false);
        state.num.bValUpdated.resize(NUM_LABELS, false);
        
        // Read current values via LabelStateAndBookkeeping public methods.
        for (int i = 0; i < NUM_LABELS; i++) {
            Labels label = static_cast<Labels>(i);
            state.num.val[i] = m_labelTracker.currVal(label);
            // Mark as used if it's ADC-relevant or its value is non-zero.
            state.num.bValUsed[i] = (i <= LAST_ADC_RELEVANT_LABEL) || (state.num.val[i] != 0);
        }
        
        return state;
    }
    
};
