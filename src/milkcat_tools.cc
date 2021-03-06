//
// The MIT License (MIT)
//
// Copyright 2013-2014 The MilkCat Project Developers
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//
// mkgram.cc --- Created at 2013-10-21
// (with) mkdict.cc --- Created at 2013-06-08
// mk_model.cc -- Created at 2013-11-08
// mctools.cc -- Created at 2014-02-21
//

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <stdint.h>
#include <assert.h>
#include <unistd.h>
#include <map>
#include <string>
#include <algorithm>
#include <set>
#include "common/reimu_trie.h"
#include "common/static_array.h"
#include "common/static_hashtable.h"
#include "ml/crf_model.h"
#include "ml/crf_tagger.h"
#include "ml/perceptron_model.h"
#include "include/milkcat.h"
#include "parser/beam_yamada_parser.h"
#include "parser/dependency_parser.h"
#include "parser/feature_template.h"
#include "parser/yamada_parser.h"
#include "tagger/crf_part_of_speech_tagger.h"
#include "tagger/hmm_part_of_speech_tagger.h"
#include "util/readable_file.h"
#include "util/util.h"
#include "util/writable_file.h"

namespace milkcat {

#define UNIGRAM_INDEX_FILE "unigram.idx"
#define UNIGRAM_DATA_FILE "unigram.bin"
#define BIGRAM_FILE "bigram.bin"
#define HMM_MODEL_FILE "hmm_model.bin"

// Load unigram data from unigram_file, if an error occured set status !=
// Status::OK()
void ReadUnigramFile(const char *unigram_file,
                     std::map<std::string, double> *unigram_data,
                     Status *status) {
  ReadableFile *fd = ReadableFile::New(unigram_file, status);
  char word[1024], line[1024];
  double count, sum = 0;

  while (status->ok() && !fd->Eof()) {
    fd->ReadLine(line, sizeof(line), status);
    if (status->ok()) {
      sscanf(line, "%s %lf", word, &count);
      (*unigram_data)[std::string(word)] += count;
      sum += count;
    }
  }

  // Calculate the weight = -log(freq / total)
  if (status->ok()) {
    for (std::map<std::string, double>::iterator
         it = unigram_data->begin(); it != unigram_data->end(); ++it) {
      it->second = -log(it->second / sum);
    }
  }

  delete fd;
}

// Load bigram data from bigram_file, if an error occured set status !=
// Status::OK()
void ReadBigramFile(const char *bigram_file,
                    std::map<std::pair<std::string, std::string>, int> *bigram_data,
                    int *total_count,
                    Status *status) {
  char left[1024], right[1024], line[1024];
  int count;

  ReadableFile *fd = ReadableFile::New(bigram_file, status);
  *total_count = 0;

  while (status->ok() && !fd->Eof()) {
    fd->ReadLine(line, sizeof(line), status);
    if (status->ok()) {
      sscanf(line, "%s %s %d", left, right, &count);
      (*bigram_data)[std::pair<std::string, std::string>(left, right)] += count;
      *total_count += count;
    }
  }

  delete fd;
}

// Build Double-Array TrieTree index from unigram, and save the index and the
// unigram data file
void BuildAndSaveUnigramData(const std::map<std::string, double> &unigram_data,
                             ReimuTrie *index,
                             Status *status) {
  std::vector<float> weight;

  // term_id = 0 is reserved for out-of-vocabulary word
  weight.push_back(0.0);

  for (std::map<std::string, double>::const_iterator
       it = unigram_data.begin(); it != unigram_data.end(); ++it) {
    index->Put(it->first.c_str(), weight.size());
    weight.push_back(it->second);
  }

  WritableFile *fd = NULL;
  if (status->ok()) fd = WritableFile::New(UNIGRAM_DATA_FILE, status);
  if (status->ok())
    fd->Write(weight.data(), sizeof(float) * weight.size(), status);
  delete fd;

  if (status->ok()) {
    bool success = index->Save(UNIGRAM_INDEX_FILE);
    if (!success) {
      std::string errmsg = "Unable to save unigram index data: ";
      errmsg += UNIGRAM_INDEX_FILE;
      *status = Status::IOError(errmsg.c_str());
    } 
  }
}

// Save unigram data into binary file UNIGRAM_FILE. On success, return the
// number of bigram word pairs successfully writed. On failed, set status !=
// Status::OK()
int SaveBigramBinFile(
    const std::map<std::pair<std::string, std::string>, int> &bigram_data,
    int total_count,
    ReimuTrie *index,
    Status *status) {
  const char *left_word, *right_word;
  int32_t left_id, right_id;
  int count;
  std::vector<int64_t> keys;
  std::vector<float> values;

  for (std::map<std::pair<std::string, std::string>, int>::const_iterator
       it = bigram_data.begin(); it != bigram_data.end(); ++it) {
    left_word = it->first.first.c_str();
    right_word = it->first.second.c_str();
    count = it->second;
    left_id = index->Get(left_word, -1);
    right_id = index->Get(right_word, -1);
    if (left_id > 0 && right_id > 0) {
      keys.push_back((static_cast<int64_t>(left_id) << 32) + right_id);
      values.push_back(-log(static_cast<double>(count) / total_count));
    }
  }

  const StaticHashTable<int64_t, float> *
  hashtable = StaticHashTable<int64_t, float>::Build(
      keys.data(),
      values.data(),
      keys.size());
  hashtable->Save(BIGRAM_FILE, status);

  delete hashtable;
  return keys.size();
}

int MakeGramModel(int argc, char **argv) {
  ReimuTrie *index = new ReimuTrie();
  std::map<std::string, double> unigram_data;
  std::map<std::pair<std::string, std::string>, int> bigram_data;
  Status status;

  if (argc != 4) {
    status = Status::Info(
      "Usage: milkcat-tools gram [UNIGRAM FILE] [BIGRAM FILE]");
  }

  const char *unigram_file = argv[argc - 2];
  const char *bigram_file = argv[argc - 1];

  if (status.ok()) {
    printf("Loading unigram data ...");
    fflush(stdout);
    ReadUnigramFile(unigram_file, &unigram_data, &status);
  }

  int total_count = 0;
  if (status.ok()) {
    printf(" OK, %d entries loaded.\n", static_cast<int>(unigram_data.size()));
    printf("Loading bigram data ...");
    fflush(stdout);
    ReadBigramFile(bigram_file, &bigram_data, &total_count, &status);
  }

  if (status.ok()) {
    printf(" OK, %d entries loaded.\n", static_cast<int>(bigram_data.size()));
    printf("Saveing unigram index and data file ...");
    fflush(stdout);
    BuildAndSaveUnigramData(unigram_data, index, &status);
  }

  int count = 0;
  if (status.ok()) {
    printf(" OK\n");
    printf("Saving Bigram Binary File ...");
    count = SaveBigramBinFile(bigram_data, total_count, index, &status);
  }

  delete index;
  if (status.ok()) {
    printf(" OK, %d entries saved.\n", count);
    printf("Success!");
    return 0;
  } else {
    puts(status.what());
    return -1;
  }
}

int MakeIndexFile(int argc, char **argv) {
  if (argc != 4) {
    fprintf(stderr, "Usage: milkcat-tools dict [INPUT-FILE] [OUTPUT-FILE]\n");
    return 1;
  }

  const char *input_path = argv[argc - 2];
  const char *output_path = argv[argc - 1];

  ReimuTrie *index = new ReimuTrie();

  FILE *fd = fopen(input_path, "r");
  if (fd == NULL) {
    fprintf(stderr, "error: unable to open input file %s\n", input_path);
    return 1;
  }

  char key_text[1024];
  int value = 0;
  int count = 0;
  while (fscanf(fd, "%s %d", key_text, &value) != EOF) {
    index->Put(key_text, value);
    ++count;
  }
  if (index->Save(output_path) == true) {
    printf("save %d words.\n", count);
  } else {
    puts("An error occured");
  }

  fclose(fd);
  delete index;
  return 0;
}

int MakePerceptronFile(int argc, char **argv) {
  Status status;

  if (argc != 4)
    status = Status::Info("Usage: milkcat-tools perc "
                          "text-model-file binary-model-file");

  printf("Load text formatted model: %s \n", argv[argc - 2]);
  
  PerceptronModel *
  perc = PerceptronModel::OpenText(argv[argc - 2], &status);

  if (status.ok()) {
    printf("Save binary formatted model: %s \n", argv[argc - 1]);
    perc->Save(argv[argc - 1], &status);
  }

  delete perc;
  if (status.ok()) {
    return 0;
  } else {
    puts(status.what());
    return -1;
  }
}

void DisplayProgress(int64_t bytes_processed,
                     int64_t file_size,
                     int64_t bytes_per_second) {
  fprintf(stderr,
          "\rprogress %dMB/%dMB -- %2.1f%% %.3fMB/s",
          static_cast<int>(bytes_processed / (1024 * 1024)),
          static_cast<int>(file_size / (1024 * 1024)),
          100.0 * bytes_processed / file_size,
          bytes_per_second / static_cast<double>(1024 * 1024));
}

int TrainDependendyParser(int argc, char **argv) {
  if (argc != 7) {
    fprintf(stderr,
            "Usage: milkcat-tools depparser-train corpus_file template_file "
            "model_file beam_size iteration\n");
    return 1;
  }
  const char *corpus_file = argv[2];
  const char *template_file = argv[3];
  const char *model_prefix = argv[4];
  int beam_size = atol(argv[5]);
  int max_iteration = atol(argv[6]);

  Status status;
  BeamYamadaParser::Train(
      corpus_file,
      template_file,
      model_prefix,
      beam_size,
      max_iteration,
      &status);

  if (status.ok()) {
    puts("Success!");
    return 0;
  } else {
    puts(status.what());
    return 1;
  }
}

int TestDependendyParser(int argc, char **argv) {
  if (argc != 6) {
    fprintf(stderr,
            "Usage: milkcat-tools depparser-test corpus_file template_file "
            "model_file beam_size\n");
    return 1;
  }
  const char *corpus_file = argv[2];
  const char *template_file = argv[3];
  const char *model_prefix = argv[4];
  int beam_size = atol(argv[5]);
  char filename[2048];

  Status status;
  DependencyParser::FeatureTemplate *
  feature = DependencyParser::FeatureTemplate::Open(template_file, &status);

  PerceptronModel *model = NULL;
  if (status.ok()) {
    model = PerceptronModel::Open(model_prefix, &status);
  } 

  BeamYamadaParser *parser = NULL;
  double LAS, UAS;
  if (status.ok()) {
    parser = new BeamYamadaParser(model, feature, beam_size);
    // parser = new NaiveArceagerDependencyParser(model, feature);
    DependencyParser::Test(
        corpus_file,
        parser,
        &LAS,
        &UAS,
        &status);
  }

  if (status.ok()) {
    printf("LAS: %lf\n", LAS);
    printf("UAS: %lf\n", UAS);
  }

  if (!status.ok()) puts(status.what());

  delete feature;
  delete model;
  delete parser;
  return 0;
}

int TestPartOfSpeechTagger(int argc, char **argv) {
  if (argc != 4) {
    fprintf(stderr,
            "Usage: milkcat-tools postagger-test corpus_file model_file\n");
    return 1;
  }
  const char *corpus_file = argv[2];
  const char *model_file = argv[3];

  Status status;

  CRFPartOfSpeechTagger *tagger = NULL;
  CRFModel *model = CRFModel::New(model_file, &status);
  if (status.ok()) {
    tagger = CRFPartOfSpeechTagger::New(model, NULL, &status);
  }
  double ta = 0.0;
  if (status.ok()) {
    ta = PartOfSpeechTagger::Test(corpus_file, tagger, &status);
  }

  if (!status.ok()) {
    puts(status.what());
  } else {
    printf("TA = %5.4f\n", ta);
  }

  delete tagger;
  delete model;

  return 0;
}

int TrainHmmPartOfSpeechTagger(int argc, char **argv) {
  if (argc != 5) {
    fprintf(stderr,
            "Usage: milkcat-tools postagger-train hmm corpus_file"
            " model_file\n");
    return 1;
  }
  const char *corpus_file = argv[3];  
  const char *model_file = argv[4];

  Status status;
  HMMPartOfSpeechTagger::Train(corpus_file, model_file, &status);
  if (!status.ok()) {
    puts(status.what());
    return 1;
  } else {
    return 0;
  }
}

int WapitiConvert(int argc, char **argv) {
  if (argc != 5) {
    fprintf(stderr,
            "Usage: milkcat-tools wapiti-conv wapiti_dump_file "
            " template_file model_file\n");
    return 1;
  }

  const char *wapiti_file = argv[2];  
  const char *template_file = argv[3];
  const char *model_prefix = argv[4];

  Status status;
  CRFModel *model = CRFModel::OpenText(wapiti_file, template_file, &status);
  if (status.ok()) model->Save(model_prefix, &status);

  if (!status.ok()) {
    puts(status.what());
    return 1;
  } else {
    return 0;
  }
}

}  // namespace milkcat

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "Usage: milkcat-tools [dict|gram|perc|depparser-train|"
                    "depparser-test|postagger-test|postagger-train|"
                    "wapiti-conv]\n");
    return 1;
  }

  char *tool = argv[1];

  if (strcmp(tool, "dict") == 0) {
    return milkcat::MakeIndexFile(argc, argv);
  } else if (strcmp(tool, "gram") == 0) {
    return milkcat::MakeGramModel(argc, argv);
  } else if (strcmp(tool, "perc") == 0) {
    return milkcat::MakePerceptronFile(argc, argv);
  } else if (strcmp(tool, "depparser-train") == 0) {
    return milkcat::TrainDependendyParser(argc, argv);
  } else if (strcmp(tool, "depparser-test") == 0) {
    return milkcat::TestDependendyParser(argc, argv);
  } else if (strcmp(tool, "postagger-test") == 0) {
    return milkcat::TestPartOfSpeechTagger(argc, argv);
  } else if (strcmp(tool, "postagger-train") == 0) {
    return milkcat::TrainHmmPartOfSpeechTagger(argc, argv);  
  } else if (strcmp(tool, "wapiti-conv") == 0) {
    return milkcat::WapitiConvert(argc, argv);
  } else {
    fprintf(stderr, "Usage: milkcat-tools [dict|gram|perc|depparser-train|"
                    "depparser-test|postagger-test|postagger-train|"
                    "wapiti-conv]\n");
    return 1;
  }

  return 0;
}