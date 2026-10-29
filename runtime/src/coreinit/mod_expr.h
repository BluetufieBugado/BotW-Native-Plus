// Expression evaluator for Cemu graphic pack rules.txt and patches.txt.
//
// Arithmetic (+ - * / %), comparisons, logical and bitwise operators, ternary ?:, and the
// functions floor, ceil, round, trunc, abs, sqrt, min, max and clamp, over $variables.
//
// Header only and free of any runtime or Win32 dependency, so the game (coreinit/mods.cpp) and
// the launcher (launcher/mods.cpp) evaluate [Preset] conditions and values the same way.
#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mods::expr {

inline std::string lower_ascii(std::string_view sv) {
    std::string out(sv);
    for (char& ch : out)
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    return out;
}

class ExprParser {
public:
    ExprParser(std::string_view src, const std::vector<std::pair<std::string, double>>& vars, bool unknown_zero)
        : s_(src), vars_(vars), unknown_zero_(unknown_zero) {}

    bool parse(double& out) {
        skip_ws();
        if (pos_ >= s_.size()) return false;
        double v = parse_ternary();
        skip_ws();
        if (!ok_ || pos_ != s_.size()) return false;
        out = v;
        return true;
    }

private:
    std::string_view s_;
    const std::vector<std::pair<std::string, double>>& vars_;
    bool unknown_zero_ = false;
    size_t pos_ = 0;
    bool ok_ = true;

    void skip_ws() {
        while (pos_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[pos_]))) pos_++;
    }

    bool match(std::string_view tok) {
        skip_ws();
        if (s_.substr(pos_, tok.size()) == tok) {
            pos_ += tok.size();
            return true;
        }
        return false;
    }

    double parse_ternary() {
        double cond = parse_or();
        skip_ws();
        if (pos_ < s_.size() && s_[pos_] == '?') {
            pos_++;
            double t_val = parse_ternary();
            if (!match(":")) {
                ok_ = false;
                return 0.0;
            }
            double f_val = parse_ternary();
            return (std::fabs(cond) > 1e-12) ? t_val : f_val;
        }
        return cond;
    }

    double parse_or() {
        double lhs = parse_and();
        while (ok_) {
            skip_ws();
            if (s_.substr(pos_, 2) == "||") {
                pos_ += 2;
                double rhs = parse_and();
                lhs = (std::fabs(lhs) > 1e-12 || std::fabs(rhs) > 1e-12) ? 1.0 : 0.0;
            } else {
                break;
            }
        }
        return lhs;
    }

    double parse_and() {
        double lhs = parse_bit_or();
        while (ok_) {
            skip_ws();
            if (s_.substr(pos_, 2) == "&&") {
                pos_ += 2;
                double rhs = parse_bit_or();
                lhs = (std::fabs(lhs) > 1e-12 && std::fabs(rhs) > 1e-12) ? 1.0 : 0.0;
            } else {
                break;
            }
        }
        return lhs;
    }

    double parse_bit_or() {
        double lhs = parse_bit_xor();
        while (ok_) {
            skip_ws();
            if (pos_ < s_.size() && s_[pos_] == '|' && s_.substr(pos_, 2) != "||") {
                pos_++;
                double rhs = parse_bit_xor();
                lhs = static_cast<double>(static_cast<int64_t>(lhs) | static_cast<int64_t>(rhs));
            } else {
                break;
            }
        }
        return lhs;
    }

    double parse_bit_xor() {
        double lhs = parse_bit_and();
        while (ok_) {
            skip_ws();
            if (pos_ < s_.size() && s_[pos_] == '^') {
                pos_++;
                double rhs = parse_bit_and();
                lhs = static_cast<double>(static_cast<int64_t>(lhs) ^ static_cast<int64_t>(rhs));
            } else {
                break;
            }
        }
        return lhs;
    }

    double parse_bit_and() {
        double lhs = parse_eq();
        while (ok_) {
            skip_ws();
            if (pos_ < s_.size() && s_[pos_] == '&' && s_.substr(pos_, 2) != "&&") {
                pos_++;
                double rhs = parse_eq();
                lhs = static_cast<double>(static_cast<int64_t>(lhs) & static_cast<int64_t>(rhs));
            } else {
                break;
            }
        }
        return lhs;
    }

    double parse_eq() {
        double lhs = parse_rel();
        while (ok_) {
            skip_ws();
            if (match("==")) {
                double rhs = parse_rel();
                lhs = (std::fabs(lhs - rhs) <= 1e-9) ? 1.0 : 0.0;
            } else if (match("!=")) {
                double rhs = parse_rel();
                lhs = (std::fabs(lhs - rhs) > 1e-9) ? 1.0 : 0.0;
            } else {
                break;
            }
        }
        return lhs;
    }

    double parse_rel() {
        double lhs = parse_shift();
        while (ok_) {
            skip_ws();
            if (match("<=")) {
                double rhs = parse_shift();
                lhs = (lhs <= rhs + 1e-9) ? 1.0 : 0.0;
            } else if (match(">=")) {
                double rhs = parse_shift();
                lhs = (lhs + 1e-9 >= rhs) ? 1.0 : 0.0;
            } else if (match("<")) {
                double rhs = parse_shift();
                lhs = (lhs < rhs - 1e-9) ? 1.0 : 0.0;
            } else if (match(">")) {
                double rhs = parse_shift();
                lhs = (lhs > rhs + 1e-9) ? 1.0 : 0.0;
            } else {
                break;
            }
        }
        return lhs;
    }

    double parse_shift() {
        double lhs = parse_add();
        while (ok_) {
            skip_ws();
            if (match("<<")) {
                double rhs = parse_add();
                lhs = static_cast<double>(static_cast<int64_t>(lhs) << static_cast<int>(rhs));
            } else if (match(">>")) {
                double rhs = parse_add();
                lhs = static_cast<double>(static_cast<int64_t>(lhs) >> static_cast<int>(rhs));
            } else {
                break;
            }
        }
        return lhs;
    }

    double parse_add() {
        double lhs = parse_mul();
        while (ok_) {
            skip_ws();
            if (pos_ < s_.size() && s_[pos_] == '+') {
                pos_++;
                lhs += parse_mul();
            } else if (pos_ < s_.size() && s_[pos_] == '-') {
                pos_++;
                lhs -= parse_mul();
            } else {
                break;
            }
        }
        return lhs;
    }

    double parse_mul() {
        double lhs = parse_unary();
        while (ok_) {
            skip_ws();
            if (pos_ < s_.size() && s_[pos_] == '*') {
                pos_++;
                lhs *= parse_unary();
            } else if (pos_ < s_.size() && s_[pos_] == '/') {
                pos_++;
                double rhs = parse_unary();
                if (std::fabs(rhs) < 1e-15) {
                    ok_ = false;
                    return 0.0;
                }
                lhs /= rhs;
            } else if (pos_ < s_.size() && s_[pos_] == '%') {
                pos_++;
                double rhs = parse_unary();
                if (std::fabs(rhs) < 1e-15) {
                    ok_ = false;
                    return 0.0;
                }
                lhs = std::fmod(lhs, rhs);
            } else {
                break;
            }
        }
        return lhs;
    }

    double parse_unary() {
        skip_ws();
        if (pos_ < s_.size()) {
            if (s_[pos_] == '+') {
                pos_++;
                return parse_unary();
            }
            if (s_[pos_] == '-') {
                pos_++;
                return -parse_unary();
            }
            if (s_[pos_] == '!' && s_.substr(pos_, 2) != "!=") {
                pos_++;
                double v = parse_unary();
                return (std::fabs(v) <= 1e-12) ? 1.0 : 0.0;
            }
            if (s_[pos_] == '~') {
                pos_++;
                double v = parse_unary();
                return static_cast<double>(~static_cast<int64_t>(v));
            }
        }
        return parse_primary();
    }

    double lookup_var(std::string_view name) {
        if (!name.empty() && name[0] == '$') name.remove_prefix(1);
        for (auto it = vars_.rbegin(); it != vars_.rend(); ++it) {
            std::string_view k = it->first;
            if (!k.empty() && k[0] == '$') k.remove_prefix(1);
            if (k == name) return it->second;
        }
        if (unknown_zero_) return 0.0;
        ok_ = false;
        return 0.0;
    }

    double parse_primary() {
        skip_ws();
        if (pos_ >= s_.size()) {
            ok_ = false;
            return 0.0;
        }
        if (s_[pos_] == '(') {
            pos_++;
            double v = parse_ternary();
            if (!match(")")) {
                ok_ = false;
                return 0.0;
            }
            return v;
        }
        if (s_[pos_] == '$') {
            pos_++;
            size_t start = pos_;
            while (pos_ < s_.size() &&
                   (std::isalnum(static_cast<unsigned char>(s_[pos_])) || s_[pos_] == '_')) {
                pos_++;
            }
            if (pos_ == start) {
                ok_ = false;
                return 0.0;
            }
            std::string_view name = s_.substr(start, pos_ - start);
            // In Cemu rules.txt, variables can have a suffix like $var:int
            if (pos_ < s_.size() && s_[pos_] == ':') {
                size_t save = pos_;
                pos_++;
                size_t ts = pos_;
                while (pos_ < s_.size() && std::isalpha(static_cast<unsigned char>(s_[pos_]))) pos_++;
                std::string_view tag = s_.substr(ts, pos_ - ts);
                if (tag != "int" && tag != "float" && tag != "double") {
                    pos_ = save;
                }
            }
            return lookup_var(name);
        }
        if (std::isalpha(static_cast<unsigned char>(s_[pos_])) || s_[pos_] == '_') {
            size_t start = pos_;
            while (pos_ < s_.size() &&
                   (std::isalnum(static_cast<unsigned char>(s_[pos_])) || s_[pos_] == '_')) {
                pos_++;
            }
            std::string ident = lower_ascii(s_.substr(start, pos_ - start));
            std::string_view raw_ident = s_.substr(start, pos_ - start);
            skip_ws();
            if (pos_ < s_.size() && s_[pos_] == '(') {
                pos_++;
                std::vector<double> args;
                skip_ws();
                if (pos_ < s_.size() && s_[pos_] != ')') {
                    while (ok_) {
                        args.push_back(parse_ternary());
                        skip_ws();
                        if (match(",")) continue;
                        break;
                    }
                }
                if (!match(")")) {
                    ok_ = false;
                    return 0.0;
                }
                if (ident == "floor" && args.size() == 1) return std::floor(args[0]);
                if (ident == "ceil" && args.size() == 1) return std::ceil(args[0]);
                if (ident == "round" && args.size() == 1) return std::round(args[0]);
                if (ident == "trunc" && args.size() == 1) return std::trunc(args[0]);
                if (ident == "abs" && args.size() == 1) return std::fabs(args[0]);
                if (ident == "sqrt" && args.size() == 1) return args[0] >= 0.0 ? std::sqrt(args[0]) : 0.0;
                if (ident == "min" && args.size() == 2) return std::min(args[0], args[1]);
                if (ident == "max" && args.size() == 2) return std::max(args[0], args[1]);
                if (ident == "clamp" && args.size() == 3) return std::clamp(args[0], args[1], args[2]);
                ok_ = false;
                return 0.0;
            }
            if (ident == "true") return 1.0;
            if (ident == "false") return 0.0;
            return lookup_var(raw_ident);
        }
        // Number literal (hex 0x... or decimal/float)
        if (pos_ + 2 <= s_.size() && s_[pos_] == '0' && (s_[pos_ + 1] == 'x' || s_[pos_ + 1] == 'X')) {
            pos_ += 2;
            size_t start = pos_;
            uint64_t val = 0;
            while (pos_ < s_.size() && std::isxdigit(static_cast<unsigned char>(s_[pos_]))) {
                char c = s_[pos_++];
                uint64_t d = (c >= '0' && c <= '9') ? (c - '0')
                           : (c >= 'a' && c <= 'f') ? (c - 'a' + 10)
                                                    : (c - 'A' + 10);
                val = (val << 4) | d;
            }
            if (pos_ == start) {
                ok_ = false;
                return 0.0;
            }
            return static_cast<double>(val);
        }
        size_t start = pos_;
        bool seen_digit = false;
        while (pos_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[pos_]))) {
            seen_digit = true;
            pos_++;
        }
        if (pos_ < s_.size() && s_[pos_] == '.') {
            pos_++;
            while (pos_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[pos_]))) {
                seen_digit = true;
                pos_++;
            }
        }
        if (!seen_digit) {
            ok_ = false;
            return 0.0;
        }
        if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
            pos_++;
            if (pos_ < s_.size() && (s_[pos_] == '+' || s_[pos_] == '-')) pos_++;
            while (pos_ < s_.size() && std::isdigit(static_cast<unsigned char>(s_[pos_]))) pos_++;
        }
        std::string num_str(s_.substr(start, pos_ - start));
        // Skip optional 'f' suffix on float literals
        if (pos_ < s_.size() && (s_[pos_] == 'f' || s_[pos_] == 'F')) pos_++;
        char* endptr = nullptr;
        double val = std::strtod(num_str.c_str(), &endptr);
        if (!endptr || *endptr != '\0') {
            ok_ = false;
            return 0.0;
        }
        return val;
    }
};

// Evaluates expr with the given variables. With unknown_zero an unknown $variable reads as 0,
// otherwise it makes the whole expression fail.
inline bool eval(std::string_view expr, const std::vector<std::pair<std::string, double>>& vars,
                 double* out_val = nullptr, bool unknown_zero = false) {
    ExprParser p(expr, vars, unknown_zero);
    double v = 0.0;
    if (!p.parse(v)) return false;
    if (out_val) *out_val = v;
    return true;
}

}  // namespace mods::expr
