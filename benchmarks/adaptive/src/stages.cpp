#include "pipeline_api.h"

namespace pipeline {

// ---- Pipeline 1: process stages ----

int load(int data) {
    return data * 2;
}

int enhance(int pixels) {
    // Heavy computation: ~5ms
    volatile int sum = pixels;
    for (int i = 0; i < 5000000; ++i)
        sum += i % 7;
    return static_cast<int>(sum);
}

int detect(int pixels) {
    // Light after first run (cache-friendly): ~0.003ms
    return pixels + 1;
}

int merge(int a, int b) {
    return a + b;
}

// ---- Pipeline 2: analyze stages ----

int parse(int data) {
    return data * 3;
}

int classify(int tokens) {
    // Moderate computation
    volatile int sum = tokens;
    for (int i = 0; i < 3000000; ++i)
        sum += i % 11;
    return static_cast<int>(sum);
}

int score(int tokens) {
    return tokens + 7;
}

int summarize(int a, int b) {
    return a + b;
}

// ---- Pipeline 3: transform stages ----

int decode(int data) {
    return data * 5;
}

int normalize(int values) {
    // Heavy computation (different profile from enhance)
    volatile int sum = values;
    for (int i = 0; i < 4000000; ++i)
        sum += i % 13;
    return static_cast<int>(sum);
}

int quantize(int values) {
    return values + 3;
}

int encode(int a, int b) {
    return a + b;
}

// ---- Pipeline 4: compress stages ----

int scan(int data) {
    return data * 4;
}

int deduplicate(int blocks) {
    // Moderate-heavy computation
    volatile int sum = blocks;
    for (int i = 0; i < 2000000; ++i)
        sum += i % 17;
    return static_cast<int>(sum);
}

int pack(int blocks) {
    return blocks + 11;
}

int finalize(int a, int b) {
    return a + b;
}

} // namespace pipeline
