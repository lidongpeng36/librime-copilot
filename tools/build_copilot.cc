//
// Copyright RIME Developers
//
#include <rime/common.h>
#include <algorithm>
#include <iostream>
#include "copilot_db.h"

using namespace rime;

int main(int argc, char* argv[]) {
  rime::copilot::RawData data;
  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.empty()) break;  // 空行，退出循环

    std::istringstream iss(line);
    std::string key;
    rime::copilot::RawEntry entry;
    if (!(iss >> key >> entry.text >> entry.weight)) {
      std::cerr << "格式错误: " << line << std::endl;
      continue;
    }
    data[key].push_back(std::move(entry));
  }
  /*
  while (std::cin) {
    string key;
    std::cin >> key;
    if (key.empty())
      break;
    rime::copilot::RawEntry entry;
    std::cin >> entry.text >> entry.weight;
    data[key].push_back(std::move(entry));
  }
  */

  path file_path = argc > 1 ? path(argv[1]) : path{"copilot.db"};
  CopilotDb db(file_path);
  LOG(INFO) << "creating " << db.file_path();
  if (!db.Build(data) || !db.Save()) {
    LOG(ERROR) << "failed to build " << db.file_path();
    return 1;
  }
  LOG(INFO) << "created: " << db.file_path();
  return 0;
}
