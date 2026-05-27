#ifndef PIPELINE_API_H
#define PIPELINE_API_H

namespace pipeline {
// Pipeline 1: process (original)
int process(int data);
int load(int data);
int enhance(int pixels);
int detect(int pixels);
int merge(int a, int b);

// Pipeline 2: analyze
int analyze(int data);
int parse(int data);
int classify(int tokens);
int score(int tokens);
int summarize(int a, int b);

// Pipeline 3: transform
int transform(int data);
int decode(int data);
int normalize(int values);
int quantize(int values);
int encode(int a, int b);

// Pipeline 4: compress
int compress(int data);
int scan(int data);
int deduplicate(int blocks);
int pack(int blocks);
int finalize(int a, int b);
} // namespace pipeline

#endif
