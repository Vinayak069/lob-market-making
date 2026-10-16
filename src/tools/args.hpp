#pragma once
// Minimal --key value argument parsing for the command-line tools.
#include <cstdlib>
#include <map>
#include <stdexcept>
#include <string>

class Args {
public:
    Args(int argc, char** argv) {
        for (int i = 1; i < argc; ++i) {
            std::string k = argv[i];
            if (k.rfind("--", 0) != 0 || i + 1 >= argc) throw std::invalid_argument("expected --key value, got " + k);
            kv_[k.substr(2)] = argv[++i];
        }
    }
    bool has(const std::string& k) const { return kv_.count(k) > 0; }
    std::string str(const std::string& k) const {
        auto it = kv_.find(k);
        if (it == kv_.end()) throw std::invalid_argument("missing --" + k);
        return it->second;
    }
    double num(const std::string& k, double def) const { return has(k) ? std::stod(str(k)) : def; }

private:
    std::map<std::string, std::string> kv_;
};
