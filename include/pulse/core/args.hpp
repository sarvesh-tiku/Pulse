#pragma once

#include <cstdlib>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace pulse {

// Minimal "--key value" / "--flag" / positional argument parser for the tools.
class Args {
 public:
  Args(int argc, char** argv, int first = 1) {
    for (int i = first; i < argc; ++i) {
      std::string a = argv[i];
      if (a.rfind("--", 0) == 0) {
        std::string key = a.substr(2);
        const auto eq = key.find('=');
        if (eq != std::string::npos) {
          kv_[key.substr(0, eq)] = key.substr(eq + 1);
        } else if (i + 1 < argc && std::string(argv[i + 1]).rfind("--", 0) != 0) {
          kv_[key] = argv[++i];
        } else {
          kv_[key] = "";
        }
      } else {
        pos_.push_back(a);
      }
    }
  }

  bool has(const std::string& k) const { return kv_.count(k) != 0; }
  std::string str(const std::string& k, const std::string& def = "") const {
    auto it = kv_.find(k);
    return it == kv_.end() ? def : it->second;
  }
  double num(const std::string& k, double def) const {
    auto it = kv_.find(k);
    if (it == kv_.end()) return def;
    char* end = nullptr;
    // Accept suffixes: 10M, 500k, 1x
    const double v = std::strtod(it->second.c_str(), &end);
    if (end == it->second.c_str()) throw std::invalid_argument("--" + k + ": not a number: " + it->second);
    std::string suf = end;
    if (suf == "k" || suf == "K") return v * 1e3;
    if (suf == "m" || suf == "M") return v * 1e6;
    if (suf == "x" || suf.empty()) return v;
    throw std::invalid_argument("--" + k + ": bad suffix: " + it->second);
  }
  const std::vector<std::string>& positional() const { return pos_; }
  std::vector<std::string> keys() const {
    std::vector<std::string> v;
    for (const auto& [k, _] : kv_) v.push_back(k);
    return v;
  }

 private:
  std::map<std::string, std::string> kv_;
  std::vector<std::string> pos_;
};

}  // namespace pulse
