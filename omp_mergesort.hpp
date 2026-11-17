#pragma once

#include "record_structure.hpp"
#include <vector>
#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <omp.h>
#include <memory>
#include <mutex>
#include <queue>

class OpenMPMergeSort {
private:
    int num_threads_;
    std::mutex io_mutex_;

    struct ChunkData {
        std::vector<RecordPtr> records;
        size_t start_pos;
        size_t end_pos;
    };

public:
    OpenMPMergeSort(int threads) : num_threads_(threads) {
        if (threads <= 0) throw std::invalid_argument("Thread count must be positive");
        omp_set_num_threads(threads);
        omp_set_dynamic(0);
    }

    void sort(const std::string& input, const std::string& output) {
        Timer timer("OpenMP sort total time");
        std::ifstream in(input, std::ios::binary);
        if (!in) throw std::runtime_error("Cannot open input file");
        std::vector<ChunkData> chunks(num_threads_);
        size_t next = 0;
        while (true) {
            auto record = readRecord(in);
            if (!record.get()) break;
            chunks[next].records.emplace_back(std::move(record));
            next = (next + 1) % chunks.size();
        }

        #pragma omp parallel for num_threads(num_threads_)
        for (int tid = 0; tid < num_threads_; ++tid) {
            // Local sort
            std::sort(chunks[tid].records.begin(), chunks[tid].records.end(),
                [](const RecordPtr& a, const RecordPtr& b) {
                    return a.get()->key < b.get()->key;
                });
        }

        // Phase 2: Parallel merge
        std::vector<RecordPtr> final_sorted = kWayMerge(chunks);

        // Phase 3: Write output
        writeRecords(output, final_sorted);
    }

    // Method for sorting records in-memory (used by MPI)
    void sortRecords(std::vector<RecordPtr>& records) {
        Timer timer("OpenMP in-memory sort");
        
        // Simple sequential sort (thread-safe)
        std::sort(records.begin(), records.end(),
            [](const RecordPtr& a, const RecordPtr& b) {
                return a.get()->key < b.get()->key;
            });
    }

    // K-way merge for MPI (merges multiple sorted files)
    void kWayMerge(const std::vector<std::string>& inputFiles, const std::string& outputFile) {
        std::vector<std::ifstream> files(inputFiles.size());
        std::vector<RecordPtr> currentRecords(inputFiles.size());
        
        // Open all input files
        for (size_t i = 0; i < inputFiles.size(); ++i) {
            files[i].open(inputFiles[i], std::ios::binary);
            if (!files[i]) {
                throw std::runtime_error("Cannot open file: " + inputFiles[i]);
            }
            currentRecords[i] = readRecord(files[i]);
        }
        
        std::ofstream outFile(outputFile, std::ios::binary);
        if (!outFile) {
            throw std::runtime_error("Cannot create output file: " + outputFile);
        }
        
        // Merge using priority queue
        using HeapEntry = std::pair<uint64_t, size_t>; // key, file_index
        auto cmp = [](const HeapEntry& a, const HeapEntry& b) {
            return a.first > b.first; // min-heap
        };
        
        std::priority_queue<HeapEntry, std::vector<HeapEntry>, decltype(cmp)> heap(cmp);
        
        // Initialize heap
        for (size_t i = 0; i < inputFiles.size(); ++i) {
            if (currentRecords[i].get()) {
                heap.emplace(currentRecords[i].get()->key, i);
            }
        }
        
        // Merge process
        while (!heap.empty()) {
            auto [key, fileIndex] = heap.top();
            heap.pop();
            
            // Write the smallest record
            writeRecord(outFile, currentRecords[fileIndex]);
            
            // Read next record from the same file
            currentRecords[fileIndex] = readRecord(files[fileIndex]);
            if (currentRecords[fileIndex].get()) {
                heap.emplace(currentRecords[fileIndex].get()->key, fileIndex);
            }
        }
        
        outFile.close();
        for (auto& file : files) {
            file.close();
        }
    }

private:
    std::vector<RecordPtr> kWayMerge(std::vector<ChunkData>& chunks) {
        // Use pointers to records in the heap to avoid copying RecordPtr objects
        using HeapEntry = std::pair<Record*, size_t>;
        auto cmp = [](const HeapEntry& a, const HeapEntry& b) {
            return a.first->key > b.first->key;
        };
        
        std::priority_queue<HeapEntry, std::vector<HeapEntry>, decltype(cmp)> heap(cmp);
        std::vector<size_t> indices(chunks.size(), 0);

        // Initialize heap with pointers to first record in each chunk
        for (size_t i = 0; i < chunks.size(); ++i) {
            if (!chunks[i].records.empty()) {
                heap.emplace(chunks[i].records[0].get(), i);
                indices[i] = 1;
            }
        }

        std::vector<RecordPtr> result;
        result.reserve(chunks[0].records.size() * chunks.size());

        while (!heap.empty()) {
            auto entry = heap.top();
            heap.pop();
            
            // Find which chunk this record came from
            auto& source_chunk = chunks[entry.second];
            
            // Calculate the index within the chunk's records
            size_t record_index = indices[entry.second] - 1;
            
            // Move the RecordPtr to the result
            result.push_back(std::move(source_chunk.records[record_index]));
            
            // Push next record from the same chunk if available
            if (indices[entry.second] < chunks[entry.second].records.size()) {
                heap.emplace(chunks[entry.second].records[indices[entry.second]].get(), entry.second);
                indices[entry.second]++;
            }
        }

        return result;
    }

    void writeRecords(const std::string& output, const std::vector<RecordPtr>& records) {
        std::ofstream out(output, std::ios::binary);
        if (!out) throw std::runtime_error("Cannot open output file");

        for (const auto& r : records) {
            if (!r.get()) continue;
            writeRecord(out, r);
        }
        out.close();
        if (!out) throw std::runtime_error("Failed to write sorted output");
    }

    size_t getFileSize(const std::string& filename) {
        std::ifstream in(filename, std::ios::binary | std::ios::ate);
        if (!in) throw std::runtime_error("Cannot open file for size check");
        size_t size = in.tellg();
        in.close();
        return size;
    }
};