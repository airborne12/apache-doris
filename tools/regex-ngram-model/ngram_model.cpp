// ngram_model.cpp —— n-gram 正则索引的体积 / 过滤效果 /
// 验证耗时建模工具（一次性原型，非产品代码）
//
// 功能：
//   1. 读入语料（每行一条记录，可从 JSON 行里抽取某个字段），按
//   n（2/3/4）与粒度（字节 / Unicode 码点）切 gram；
//   2. 构建「行级」倒排（gram → docid
//   列表，行内去重），并派生「块级」倒排与「块级布隆过滤器」；
//   3. 用 Roaring / FOR-128 位打包 / VByte
//   三种口径估算倒排体积，加上词典开销，全部折算成「占原始数据字节的百分比」；
//   4. 把正则（RE2 语法子集）编译成 gram 布尔查询（Russ Cox 2012 算法的 C++
//   移植），
//      在行级 / 块级 / 布隆三种索引上求候选集，再用 re2
//      对候选做复验，测出真实耗时与「无索引全量 re2 / hyperscan」对比；
//   5. 断言：候选集 ⊇ 真实匹配集（索引只能漏杀不能误杀）。
//
// 编译（在 doris 仓库根目录）：
//   clang++ -O2 -std=c++17 -I thirdparty/installed/include ngram_model.cpp \
//       -L thirdparty/installed/lib -lroaring -lre2 -lhs -labsl_... -lpthread
//       -o ngram_model
#include <hs/hs.h>
#include <re2/re2.h>
#include <roaring/roaring.hh>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace std;
using Roaring = roaring::Roaring;

// ------------------------------------------------------------------ 选项
struct Opts {
  string corpus;
  string queries;
  string field;       // JSON 行中要抽取的字段名，空表示整行
  int n = 3;          // gram 长度
  bool cp = false;    // true = 按 Unicode 码点切 gram；false = 按字节
  bool lower = false; // 是否做 ASCII 小写归一（索引与查询同时）
  size_t max_rows = 0; // 0 = 不限
  vector<uint32_t> blocks = {1024, 4096, 16384};
  vector<int> bpp = {8, 12}; // 布隆每 gram 位数
  double drop_df =
      0.0; // >0 时：df/N 超过该比例的 gram 不建索引（查询时视为 ALL）
  bool verbose = false;
  bool cdc = false;      // true = 内容定义边界的稀疏 gram（变长），false = 稠密定长 n-gram
  double p = 0.25;       // cdc：任一字节对成为边界的概率
  int maxlen = 24;       // cdc：gram 最大字节长度
};
static unordered_map<uint64_t, string> g_gram_text;  // cdc 模式：gram 哈希 → 原文（用于打印与词典体积）
static inline uint64_t mix64(uint64_t x);
static inline bool cdc_boundary(unsigned char a, unsigned char b, const Opts &o) {
  return (mix64((((uint64_t)a << 8) | b) ^ 0x5bd1e995ULL) & 0xFFFF) < (uint64_t)(o.p * 65536);
}
static inline uint64_t fnv64(const char *s, size_t n) {
  uint64_t h = 1469598103934665603ULL;
  for (size_t i = 0; i < n; i++) { h ^= (unsigned char)s[i]; h *= 1099511628211ULL; }
  return h;
}
// cdc gram：从边界 b_k 起，延伸到首个使长度 ≥ n 的后续边界 b_j（含其字节对），上限 maxlen；
// 规则只依赖 b_k 之后的内容（局部性），故查询侧对字面量能复现出与索引侧完全相同的 gram
// 生产形态的边界判定：65536 项查表（8 KB 位图），每字节对一次查表；gram 哈希直接在原串上算，不拷贝
static vector<uint8_t> g_bnd_table; static double g_bnd_p = -1;
static inline bool cdc_boundary_fast(unsigned char a, unsigned char b) { unsigned idx = ((unsigned)a << 8) | b; return g_bnd_table[idx >> 3] >> (idx & 7) & 1; }
static void grams_of_cdc(string_view s, const Opts &o, vector<uint64_t> &out) {
  if (g_bnd_p != o.p) { g_bnd_table.assign(8192, 0); for (unsigned idx = 0; idx < 65536; idx++) if (cdc_boundary(idx >> 8, idx & 255, o)) g_bnd_table[idx >> 3] |= 1 << (idx & 7); g_bnd_p = o.p; }
  size_t L = s.size();
  if (L < (size_t)o.n) return;
  static thread_local vector<char> isb;
  isb.assign(L, 0);
  static const bool keep_text = getenv("BENCH_EXTRACT") == nullptr;
  string tmp;
  for (size_t i = 0; i + 1 < L; i++) isb[i] = cdc_boundary_fast((unsigned char)s[i], (unsigned char)s[i + 1]);
  for (size_t k = 0; k + 1 < L; k++) {
    if (!isb[k]) continue;
    size_t end = 0;
    for (size_t j = k + 1; j + 1 < L && j + 2 - k <= (size_t)o.maxlen; j++) {
      if (isb[j] && j + 2 - k >= (size_t)o.n) { end = j + 2; break; }
    }
    if (end == 0) { if (k + (size_t)o.maxlen <= L) end = k + o.maxlen; else continue; }
    uint64_t h;
    if (o.lower) { tmp.assign(s.data() + k, end - k); for (auto &ch : tmp) if (ch >= 'A' && ch <= 'Z') ch = ch - 'A' + 'a'; h = fnv64(tmp.data(), tmp.size()); }
    else h = fnv64(s.data() + k, end - k);
    out.push_back(h);
    if (keep_text && !g_gram_text.count(h)) g_gram_text.emplace(h, o.lower ? tmp : string(s.data() + k, end - k));
  }
}

// ------------------------------------------------------------------ UTF-8
static inline int utf8_len(unsigned char c) {
  if (c < 0x80)
    return 1;
  if ((c >> 5) == 0x6)
    return 2;
  if ((c >> 4) == 0xE)
    return 3;
  if ((c >> 3) == 0x1E)
    return 4;
  return 1; // 非法前导字节：按单字节处理
}
// 解码为码点序列；非法字节映射到 0x110000+byte（仍 < 2^21）
static void decode_cps(string_view s, vector<uint32_t> &out) {
  out.clear();
  size_t i = 0;
  while (i < s.size()) {
    unsigned char c = s[i];
    int l = utf8_len(c);
    if (l == 1) {
      out.push_back(c < 0x80 ? c : 0x110000 + c);
      i++;
      continue;
    }
    if (i + l > s.size()) {
      out.push_back(0x110000 + c);
      i++;
      continue;
    }
    uint32_t v = (l == 2) ? (c & 0x1F) : (l == 3) ? (c & 0x0F) : (c & 0x07);
    bool ok = true;
    for (int k = 1; k < l; k++) {
      unsigned char cc = s[i + k];
      if ((cc & 0xC0) != 0x80) {
        ok = false;
        break;
      }
      v = (v << 6) | (cc & 0x3F);
    }
    if (!ok) {
      out.push_back(0x110000 + c);
      i++;
      continue;
    }
    out.push_back(v);
    i += l;
  }
}
static void encode_cp(uint32_t cp, string &out) {
  if (cp < 0x80)
    out.push_back((char)cp);
  else if (cp < 0x800) {
    out.push_back((char)(0xC0 | (cp >> 6)));
    out.push_back((char)(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back((char)(0xE0 | (cp >> 12)));
    out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back((char)(0x80 | (cp & 0x3F)));
  } else {
    out.push_back((char)(0xF0 | (cp >> 18)));
    out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back((char)(0x80 | (cp & 0x3F)));
  }
}

// ------------------------------------------------------------------ gram 切分
// 字节模式：key = n 个字节大端拼接；码点模式：key = n 个 21 位码点拼接（n ≤ 3）
static void grams_of(string_view s, const Opts &o, vector<uint64_t> &out) {
  out.clear();
  if (o.cdc) { grams_of_cdc(s, o, out); return; }
  if (!o.cp) {
    if (s.size() < (size_t)o.n)
      return;
    uint64_t mask = (o.n >= 8) ? ~0ULL : ((1ULL << (8 * o.n)) - 1);
    uint64_t k = 0;
    for (size_t i = 0; i < s.size(); i++) {
      unsigned char c = s[i];
      if (o.lower && c >= 'A' && c <= 'Z')
        c = c - 'A' + 'a';
      k = ((k << 8) | c) & mask;
      if (i + 1 >= (size_t)o.n)
        out.push_back(k);
    }
  } else {
    static thread_local vector<uint32_t> cps;
    decode_cps(s, cps);
    if (cps.size() < (size_t)o.n)
      return;
    uint64_t mask = (1ULL << (21 * o.n)) - 1;
    uint64_t k = 0;
    for (size_t i = 0; i < cps.size(); i++) {
      uint32_t c = cps[i];
      if (o.lower && c >= 'A' && c <= 'Z')
        c = c - 'A' + 'a';
      k = ((k << 21) | c) & mask;
      if (i + 1 >= (size_t)o.n)
        out.push_back(k);
    }
  }
}
static string gram_to_str(uint64_t k, const Opts &o) {
  string s;
  if (o.cdc) { auto it = g_gram_text.find(k); return it == g_gram_text.end() ? string("?") : it->second; }
  if (!o.cp) {
    for (int i = o.n - 1; i >= 0; i--)
      s.push_back((char)((k >> (8 * i)) & 0xFF));
  } else {
    for (int i = o.n - 1; i >= 0; i--)
      encode_cp((k >> (21 * i)) & 0x1FFFFF, s);
  }
  return s;
}

// ------------------------------------------------------------------ gram
// 布尔查询
struct Q {
  enum Op { ALL, NONE, AND, OR } op = ALL;
  vector<uint64_t> grams; // 本节点直接持有的 gram（AND：全部必需；OR：任一）
  vector<Q> subs; // 子查询
  bool is_all() const { return op == ALL; }
  bool is_none() const { return op == NONE; }
};
static Q q_all() { return Q{}; }
static Q q_none() {
  Q q;
  q.op = Q::NONE;
  return q;
}
static void dedupe(vector<uint64_t> &v) {
  sort(v.begin(), v.end());
  v.erase(unique(v.begin(), v.end()), v.end());
}


static string q_key(const Q& q) {
    if (q.op == Q::ALL) return "A"; if (q.op == Q::NONE) return "N";
    string s = q.op == Q::AND ? "&[" : "|[";
    for (auto g : q.grams) s += to_string(g) + ",";
    vector<string> ks; for (auto& c : q.subs) ks.push_back(q_key(c)); sort(ks.begin(), ks.end());
    for (auto& k : ks) s += k + ";";
    return s + "]";
}
static void dedupe_subs(vector<Q>& subs) {
    set<string> seen; vector<Q> keep; if (getenv("QDBG")) { fprintf(stderr, "dedupe in=%zu:", subs.size()); for (auto& s : subs) fprintf(stderr, " [%s]", q_key(s).c_str()); fprintf(stderr, "\n"); }
    for (auto& s : subs) { string k = q_key(s); if (seen.insert(k).second) keep.push_back(std::move(s)); }
    subs = std::move(keep);
}
// OR 内：若子 AND A 的 gram 集是子 AND B 的子集，则 B 被 A 蕴含，可删
static void or_absorb_subsets(vector<Q>& subs) {
    // 先算好丢弃标记再搬移，避免在遍历中 move 掉被比较对象
    vector<char> drop(subs.size(), 0);
    for (size_t i = 0; i < subs.size(); i++) {
        if (subs[i].op != Q::AND || !subs[i].subs.empty()) continue;
        for (size_t j = 0; j < subs.size() && !drop[i]; j++) {
            if (i == j || subs[j].op != Q::AND || !subs[j].subs.empty()) continue;
            if (subs[j].grams.size() > subs[i].grams.size() || (subs[j].grams.size() == subs[i].grams.size() && j > i)) continue;
            if (includes(subs[i].grams.begin(), subs[i].grams.end(), subs[j].grams.begin(), subs[j].grams.end())) drop[i] = 1;
        }
    }
    vector<Q> keep;
    for (size_t i = 0; i < subs.size(); i++) if (!drop[i]) keep.push_back(std::move(subs[i]));
    subs = std::move(keep);
}
static Q q_and(Q a, Q b);
static Q q_or(Q a, Q b);

static Q q_and(Q a, Q b) {
  if (a.is_none() || b.is_none())
    return q_none();
  if (a.is_all())
    return b;
  if (b.is_all())
    return a;
  Q r;
  r.op = Q::AND;
  for (Q *x : {&a, &b}) {
    if (x->op == Q::AND) {
      r.grams.insert(r.grams.end(), x->grams.begin(), x->grams.end());
      for (auto &s : x->subs)
        r.subs.push_back(move(s));
    } else {
      r.subs.push_back(move(*x));
    }
  }
  dedupe(r.grams);
  // 吸收律：AND 里已有 gram g，则子 OR(...g...) 恒真，可删
  vector<Q> keep;
  for (auto &s : r.subs) {
    bool absorbed = false;
    if (s.op == Q::OR)
      for (auto g : s.grams)
        if (binary_search(r.grams.begin(), r.grams.end(), g)) {
          absorbed = true;
          break;
        }
    if (!absorbed)
      keep.push_back(move(s));
  }
  r.subs = move(keep); dedupe_subs(r.subs);
  if (r.grams.empty() && r.subs.size() == 1)
    return r.subs[0];
  return r;
}
static Q q_or(Q a, Q b) {
  if (a.is_all() || b.is_all())
    return q_all();
  if (a.is_none())
    return b;
  if (b.is_none())
    return a;
  Q r;
  r.op = Q::OR;
  for (Q *x : {&a, &b}) {
    if (x->op == Q::OR) {
      r.grams.insert(r.grams.end(), x->grams.begin(), x->grams.end());
      for (auto &s : x->subs)
        r.subs.push_back(move(s));
    } else if (x->op == Q::AND && x->grams.size() == 1 && x->subs.empty()) {
      r.grams.push_back(x->grams[0]);
    } else {
      r.subs.push_back(move(*x));
    }
  }
  dedupe(r.grams);
  // 吸收律：OR 里已有 gram g，则子 AND(...g...) 被 g 蕴含，可删
  vector<Q> keep;
  for (auto &s : r.subs) {
    bool absorbed = false;
    if (s.op == Q::AND)
      for (auto g : s.grams)
        if (binary_search(r.grams.begin(), r.grams.end(), g)) {
          absorbed = true;
          break;
        }
    if (!absorbed)
      keep.push_back(move(s));
  }
  r.subs = move(keep); dedupe_subs(r.subs); or_absorb_subsets(r.subs);
  if (r.grams.size() == 1 && r.subs.empty()) {
    Q l;
    l.op = Q::AND;
    l.grams = r.grams;
    return l;
  }
  if (r.grams.empty() && r.subs.size() == 1)
    return r.subs[0];
  return r;
}
// 字符串 s 的全部 gram 之 AND；不足 n 长则为 ALL
static Q q_of_string(const string &s, const Opts &o) {
  vector<uint64_t> g;
  grams_of(s, o, g);
  if (g.empty())
    return q_all();
  Q q;
  q.op = Q::AND;
  q.grams = g;
  dedupe(q.grams);
  return q;
}
static Q q_of_set(const set<string> &ss, const Opts &o) {
  if (ss.empty())
    return q_none();
  Q r = q_none();
  for (auto &s : ss)
    r = q_or(move(r), q_of_string(s, o));
  return r;
}
static string q_str(const Q &q, const Opts &o) {
  if (q.op == Q::ALL)
    return "ALL";
  if (q.op == Q::NONE)
    return "NONE";
  string sep = q.op == Q::AND ? " & " : " | ";
  string s = "(";
  bool first = true;
  for (auto g : q.grams) {
    if (!first)
      s += sep;
    first = false;
    s += "\"" + gram_to_str(g, o) + "\"";
  }
  for (auto &c : q.subs) {
    if (!first)
      s += sep;
    first = false;
    s += q_str(c, o);
  }
  return s + ")";
}
static int q_leaf_count(const Q &q) {
  int c = (int)q.grams.size();
  for (auto &s : q.subs)
    c += q_leaf_count(s);
  return c;
}

// ------------------------------------------------------------------ 正则 AST
struct Node {
  enum T {
    EMPTY,
    LIT,
    CLASS,
    ANY,
    CAT,
    ALT,
    STAR,
    PLUS,
    QUEST,
    REPEAT
  } t = EMPTY;
  string lit;         // LIT：一个码点的 UTF-8
  vector<string> cls; // CLASS：小类展开（每项一个码点的
                      // UTF-8）；为空表示「大类/取反类」→ 当作 ANY
  bool big_class = false;
  vector<unique_ptr<Node>> kids;
  int rmin = 0, rmax = -1; // REPEAT
};
using NP = unique_ptr<Node>;
static NP mk(Node::T t) {
  auto p = make_unique<Node>();
  p->t = t;
  return p;
}

struct Parser {
  string_view p;
  size_t i = 0;
  bool icase = false;
  bool ok = true;
  string err;
  explicit Parser(string_view s) : p(s) {}
  bool eof() const { return i >= p.size(); }
  char peek() const { return eof() ? 0 : p[i]; }
  uint32_t next_cp(string &utf8) {
    int l = utf8_len((unsigned char)p[i]);
    if (i + l > p.size())
      l = 1;
    utf8.assign(p.substr(i, l));
    vector<uint32_t> cps;
    decode_cps(utf8, cps);
    i += l;
    return cps.empty() ? 0 : cps[0];
  }
  NP parse() {
    NP r = parse_alt();
    if (!eof()) {
      ok = false;
      err = "trailing input at " + to_string(i);
    }
    return r;
  }
  NP parse_alt() {
    vector<NP> branches;
    branches.push_back(parse_cat());
    while (peek() == '|') {
      i++;
      branches.push_back(parse_cat());
    }
    if (branches.size() == 1)
      return move(branches[0]);
    NP a = mk(Node::ALT);
    a->kids = move(branches);
    return a;
  }
  NP parse_cat() {
    NP c = mk(Node::CAT);
    while (!eof() && peek() != '|' && peek() != ')') {
      NP atom = parse_atom();
      if (!ok)
        return c;
      if (!atom)
        continue; // 例如 (?i) 这种仅设标志的空原子
      atom = parse_quant(move(atom));
      c->kids.push_back(move(atom));
    }
    return c;
  }
  NP parse_quant(NP a) {
    while (!eof()) {
      char c = peek();
      if (c == '*') {
        i++;
        NP s = mk(Node::STAR);
        s->kids.push_back(move(a));
        a = move(s);
      } else if (c == '+') {
        i++;
        NP s = mk(Node::PLUS);
        s->kids.push_back(move(a));
        a = move(s);
      } else if (c == '?') {
        i++;
        NP s = mk(Node::QUEST);
        s->kids.push_back(move(a));
        a = move(s);
      } else if (c == '{') {
        size_t save = i;
        i++;
        int mn = 0, mx = -1;
        bool has = false;
        while (!eof() && isdigit(peek())) {
          mn = mn * 10 + (peek() - '0');
          i++;
          has = true;
        }
        if (!has) {
          i = save;
          break;
        }
        if (peek() == ',') {
          i++;
          if (isdigit(peek())) {
            mx = 0;
            while (!eof() && isdigit(peek())) {
              mx = mx * 10 + (peek() - '0');
              i++;
            }
          }
        } else
          mx = mn;
        if (peek() != '}') {
          i = save;
          break;
        }
        i++;
        NP s = mk(Node::REPEAT);
        s->rmin = mn;
        s->rmax = mx;
        s->kids.push_back(move(a));
        a = move(s);
      } else
        break;
      if (peek() == '?')
        i++; // 懒惰量词不影响匹配集合
    }
    return a;
  }
  // 把类里的一个码点或转义加入 out（大类置 big）
  void class_escape(vector<uint32_t> &out, bool &big) {
    char c = peek();
    i++;
    switch (c) {
    case 'd':
      big = true;
      break;
    case 'w':
      big = true;
      break;
    case 's':
      big = true;
      break;
    case 'D':
    case 'W':
    case 'S':
      big = true;
      break;
    case 'n':
      out.push_back('\n');
      break;
    case 't':
      out.push_back('\t');
      break;
    case 'r':
      out.push_back('\r');
      break;
    case 'x': {
      uint32_t v = 0;
      int cnt = 0;
      if (peek() == '{') {
        i++;
        while (!eof() && peek() != '}') {
          v = v * 16 +
              (isdigit(peek()) ? peek() - '0' : (tolower(peek()) - 'a' + 10));
          i++;
        }
        i++;
      } else {
        while (cnt < 2 && !eof() && isxdigit(peek())) {
          v = v * 16 +
              (isdigit(peek()) ? peek() - '0' : (tolower(peek()) - 'a' + 10));
          i++;
          cnt++;
        }
      }
      out.push_back(v);
      break;
    }
    case 'p':
    case 'P':
      big = true;
      if (peek() == '{') {
        while (!eof() && peek() != '}')
          i++;
        i++;
      } else
        i++;
      break;
    default: {
      i--;
      string u;
      out.push_back(next_cp(u));
      break;
    }
    }
  }
  NP parse_class() {
    // 已消费 '['
    NP n = mk(Node::CLASS);
    bool neg = false, big = false;
    if (peek() == '^') {
      neg = true;
      i++;
    }
    vector<uint32_t> items;
    bool first = true;
    while (!eof() && (peek() != ']' || first)) {
      first = false;
      if (peek() == '[' && i + 1 < p.size() &&
          p[i + 1] == ':') { // [:alpha:] 等 POSIX 类
        size_t e = p.find(":]", i);
        if (e == string::npos) {
          ok = false;
          err = "bad posix class";
          return n;
        }
        i = e + 2;
        big = true;
        continue;
      }
      uint32_t lo;
      if (peek() == '\\') {
        i++;
        size_t before = items.size();
        class_escape(items, big);
        if (items.size() == before)
          continue;
        lo = items.back();
        items.pop_back();
      } else {
        string u;
        lo = next_cp(u);
      }
      if (peek() == '-' && i + 1 < p.size() && p[i + 1] != ']') {
        i++;
        uint32_t hi;
        if (peek() == '\\') {
          i++;
          vector<uint32_t> tmp;
          bool b2 = false;
          class_escape(tmp, b2);
          hi = tmp.empty() ? lo : tmp.back();
          if (b2)
            big = true;
        } else {
          string u;
          hi = next_cp(u);
        }
        if (hi < lo)
          swap(lo, hi);
        if (hi - lo + 1 + items.size() > 4)
          big = true;
        else
          for (uint32_t x = lo; x <= hi; x++)
            items.push_back(x);
      } else
        items.push_back(lo);
    }
    if (peek() != ']') {
      ok = false;
      err = "unterminated class";
      return n;
    }
    i++;
    if (neg || big || items.size() > 4) {
      n->big_class = true;
      return n;
    }
    for (auto cp : items) {
      string u;
      encode_cp(cp, u);
      n->cls.push_back(u);
      if (icase && cp < 128 && isalpha((int)cp)) {
        string v;
        encode_cp(islower((int)cp) ? toupper((int)cp) : tolower((int)cp), v);
        n->cls.push_back(v);
      }
    }
    sort(n->cls.begin(), n->cls.end());
    n->cls.erase(unique(n->cls.begin(), n->cls.end()), n->cls.end());
    if (n->cls.size() > 4)
      n->big_class = true;
    return n;
  }
  NP make_lit(uint32_t cp) {
    string u;
    encode_cp(cp, u);
    if (icase && cp < 128 && isalpha((int)cp)) {
      NP n = mk(Node::CLASS);
      string v;
      encode_cp(islower((int)cp) ? toupper((int)cp) : tolower((int)cp), v);
      n->cls = {u, v};
      sort(n->cls.begin(), n->cls.end());
      return n;
    }
    NP n = mk(Node::LIT);
    n->lit = u;
    return n;
  }
  NP parse_atom() {
    char c = peek();
    if (c == '(') {
      i++;
      if (peek() == '?') {
        i++;
        if (peek() == ':') {
          i++;
        } else if (peek() == 'P' || peek() == '<') { // 命名组
          if (peek() == 'P')
            i++;
          if (peek() != '<') {
            ok = false;
            err = "bad group";
            return nullptr;
          }
          while (!eof() && peek() != '>')
            i++;
          i++;
        } else { // 标志 (?i) (?is) (?i:...)
          bool neg = false, anyflag = false;
          while (!eof() && peek() != ')' && peek() != ':') {
            char f = peek();
            i++;
            if (f == '-')
              neg = true;
            else if (f == 'i') {
              icase = !neg;
              anyflag = true;
            } else if (f == 's' || f == 'm' || f == 'U')
              anyflag = true;
            else {
              ok = false;
              err = "unknown flag";
              return nullptr;
            }
          }
          (void)anyflag;
          if (peek() == ')') {
            i++;
            return nullptr;
          }
          i++; // ':'
        }
      }
      NP inner = parse_alt();
      if (peek() != ')') {
        ok = false;
        err = "missing )";
        return inner;
      }
      i++;
      return inner;
    }
    if (c == '[') {
      i++;
      return parse_class();
    }
    if (c == '.') {
      i++;
      return mk(Node::ANY);
    }
    if (c == '^' || c == '$') {
      i++;
      return mk(Node::EMPTY);
    }
    if (c == '\\') {
      i++;
      if (eof()) {
        ok = false;
        err = "trailing backslash";
        return nullptr;
      }
      char e = peek();
      switch (e) {
      case 'd':
      case 'w':
      case 's':
      case 'D':
      case 'W':
      case 'S': {
        i++;
        NP n = mk(Node::CLASS);
        n->big_class = true;
        return n;
      }
      case 'b':
      case 'B':
      case 'A':
      case 'z':
        i++;
        return mk(Node::EMPTY);
      case 'p':
      case 'P': {
        i++;
        if (peek() == '{') {
          while (!eof() && peek() != '}')
            i++;
          i++;
        } else
          i++;
        NP n = mk(Node::CLASS);
        n->big_class = true;
        return n;
      }
      case 'n':
        i++;
        return make_lit('\n');
      case 't':
        i++;
        return make_lit('\t');
      case 'r':
        i++;
        return make_lit('\r');
      case 'x': {
        i++;
        vector<uint32_t> tmp;
        bool big = false;
        i--;
        class_escape(tmp, big);
        return make_lit(tmp.empty() ? 0 : tmp[0]);
      }
      case 'Q': { // \Q...\E 字面量
        i++;
        NP cat = mk(Node::CAT);
        while (!eof() &&
               !(peek() == '\\' && i + 1 < p.size() && p[i + 1] == 'E')) {
          string u;
          uint32_t cp = next_cp(u);
          cat->kids.push_back(make_lit(cp));
        }
        if (!eof())
          i += 2;
        return cat;
      }
      default: {
        string u;
        uint32_t cp = next_cp(u);
        return make_lit(cp);
      }
      }
    }
    if (c == '*' || c == '+' || c == '?') {
      ok = false;
      err = "bad quantifier";
      return nullptr;
    }
    string u;
    uint32_t cp = next_cp(u);
    return make_lit(cp);
  }
};

// ------------------------------------------------------------------ Cox 分析
static const size_t kMaxSet = 20;
struct Info {
  bool can_empty = false;
  bool has_exact = false;
  set<string> exact, prefix, suffix;
  Q match;
};
static set<string> cross(const set<string> &a, const set<string> &b,
                         bool &too_big) {
  set<string> r;
  too_big = a.size() * b.size() > kMaxSet;
  if (too_big)
    return r;
  for (auto &x : a)
    for (auto &y : b)
      r.insert(x + y);
  return r;
}
static set<string> uni(const set<string> &a, const set<string> &b) {
  set<string> r = a;
  r.insert(b.begin(), b.end());
  return r;
}
static size_t min_len(const set<string> &s) {
  size_t m = SIZE_MAX;
  for (auto &x : s)
    m = min(m, x.size());
  return m == SIZE_MAX ? 0 : m;
}
// 字符数（码点模式下按码点）
static size_t str_units(const string &s, const Opts &o) {
  if (!o.cp)
    return s.size();
  vector<uint32_t> c;
  decode_cps(s, c);
  return c.size();
}
static string head_units(const string &s, size_t k, const Opts &o) {
  if (!o.cp)
    return s.substr(0, min(k, s.size()));
  vector<uint32_t> c;
  decode_cps(s, c);
  string r;
  for (size_t i = 0; i < min(k, c.size()); i++)
    encode_cp(c[i], r);
  return r;
}
static string tail_units(const string &s, size_t k, const Opts &o) {
  if (!o.cp)
    return s.size() <= k ? s : s.substr(s.size() - k);
  vector<uint32_t> c;
  decode_cps(s, c);
  string r;
  size_t st = c.size() <= k ? 0 : c.size() - k;
  for (size_t i = st; i < c.size(); i++)
    encode_cp(c[i], r);
  return r;
}
// 把集合的 gram 折入 match（要求集合中每个串都 ≥ n，否则无信息），然后把串裁到
// n-1 个单位
static void trim_set(set<string> &s, Q &match, bool is_suffix, const Opts &o) {
  if (s.empty())
    return;
  bool all_long = true;
  for (auto &x : s)
    if (str_units(x, o) < (size_t)o.n) {
      all_long = false;
      break;
    }
  if (all_long)
    match = q_and(move(match), q_of_set(s, o));
  size_t keep = o.cdc ? (size_t)o.maxlen : (size_t)o.n - 1;
  for (;;) {
    set<string> t;
    for (auto &x : s)
      t.insert(is_suffix ? tail_units(x, keep, o) : head_units(x, keep, o));
    s = move(t);
    if (s.size() <= kMaxSet || keep == 0)
      break;
    keep--;
  }
}
// 把 exact 降级为 prefix/suffix（gram 折入 match）
static void demote(Info &x, const Opts &o) {
  if (!x.has_exact)
    return;
  bool all_long = true;
  for (auto &s : x.exact)
    if (str_units(s, o) < (size_t)o.n) {
      all_long = false;
      break;
    }
  if (all_long)
    x.match = q_and(move(x.match), q_of_set(x.exact, o));
  x.prefix = x.exact;
  x.suffix = x.exact;
  x.exact.clear();
  x.has_exact = false;
}
static Info simplify(Info x, bool force, const Opts &o) {
  if (x.has_exact) {
    size_t ml = min_len(x.exact);
    bool all_long = true;
    for (auto &s : x.exact)
      if (str_units(s, o) < (size_t)o.n) {
        all_long = false;
        break;
      }
    if (force || x.exact.size() > 7 || (all_long && x.exact.size() > 4) ||
        ml >= (size_t)(2 * o.n))
      demote(x, o);
  }
  if (!x.has_exact) {
    trim_set(x.prefix, x.match, false, o);
    trim_set(x.suffix, x.match, true, o);
  }
  return x;
}
static Info info_empty() {
  Info i;
  i.can_empty = true;
  i.has_exact = true;
  i.exact = {""};
  return i;
}
static Info info_any_char() {
  Info i;
  i.prefix = {""};
  i.suffix = {""};
  return i;
}
static Info info_any_match() {
  Info i;
  i.can_empty = true;
  i.prefix = {""};
  i.suffix = {""};
  return i;
}
static const set<string> &pre(const Info &x) {
  return x.has_exact ? x.exact : x.prefix;
}
static const set<string> &suf(const Info &x) {
  return x.has_exact ? x.exact : x.suffix;
}

static Info concat_info(Info x, Info y, const Opts &o) {
  Info r;
  if (x.has_exact && y.has_exact) {
    bool big;
    set<string> c = cross(x.exact, y.exact, big);
    if (!big) {
      r.has_exact = true;
      r.exact = move(c);
    } else {
      demote(x, o);
      demote(y, o);
    }
  }
  if (!r.has_exact) {
    if (x.has_exact) {
      bool big;
      set<string> c = cross(x.exact, y.prefix, big);
      if (big) {
        demote(x, o);
        r.prefix = x.prefix;
        if (x.can_empty)
          r.prefix = uni(r.prefix, y.prefix);
      } else
        r.prefix = move(c);
    } else {
      r.prefix = x.prefix;
      if (x.can_empty)
        r.prefix = uni(r.prefix, pre(y));
    }
    if (y.has_exact) {
      bool big;
      set<string> c = cross(x.suffix, y.exact, big);
      if (big) {
        demote(y, o);
        r.suffix = y.suffix;
        if (y.can_empty)
          r.suffix = uni(r.suffix, x.suffix);
      } else
        r.suffix = move(c);
    } else {
      r.suffix = y.suffix;
      if (y.can_empty)
        r.suffix = uni(r.suffix, suf(x));
    }
    // 边界 gram：x 的后缀 × y 的前缀 在匹配串中连续出现
    if (!x.has_exact && !y.has_exact) {
      bool big;
      set<string> c = cross(x.suffix, y.prefix, big);
      if (!big && !c.empty()) {
        bool all_long = true;
        for (auto &s : c)
          if (str_units(s, o) < (size_t)o.n) {
            all_long = false;
            break;
          }
        if (all_long)
          r.match = q_and(move(r.match), q_of_set(c, o));
      }
    }
  }
  r.match = q_and(move(r.match), q_and(move(x.match), move(y.match)));
  r.can_empty = x.can_empty && y.can_empty;
  return simplify(move(r), false, o);
}
static Info alt_info(Info x, Info y, const Opts &o) {
  Info r;
  if (x.has_exact && y.has_exact) {
    set<string> u = uni(x.exact, y.exact);
    if (u.size() <= kMaxSet) {
      r.has_exact = true;
      r.exact = move(u);
    } else {
      demote(x, o);
      demote(y, o);
    }
  }
  if (!r.has_exact) {
    demote(x, o);
    demote(y, o);
    r.prefix = uni(x.prefix, y.prefix);
    r.suffix = uni(x.suffix, y.suffix);
  }
  r.can_empty = x.can_empty || y.can_empty;
  r.match = q_or(move(x.match), move(y.match));
  return simplify(move(r), false, o);
}
static Info plus_info(Info x, const Opts &o) {
  demote(x, o);
  return simplify(move(x), false, o);
}
static Info analyze(const Node *n, const Opts &o) {
  switch (n->t) {
  case Node::EMPTY:
    return info_empty();
  case Node::LIT: {
    Info i;
    i.has_exact = true;
    i.exact = {n->lit};
    return simplify(move(i), false, o);
  }
  case Node::CLASS: {
    if (n->big_class || n->cls.empty())
      return info_any_char();
    Info i;
    i.has_exact = true;
    for (auto &s : n->cls)
      i.exact.insert(s);
    return simplify(move(i), false, o);
  }
  case Node::ANY:
    return info_any_char();
  case Node::CAT: {
    Info acc = info_empty();
    for (auto &k : n->kids)
      acc = concat_info(move(acc), analyze(k.get(), o), o);
    return acc;
  }
  case Node::ALT: {
    Info acc = analyze(n->kids[0].get(), o);
    for (size_t k = 1; k < n->kids.size(); k++)
      acc = alt_info(move(acc), analyze(n->kids[k].get(), o), o);
    return acc;
  }
  case Node::STAR:
    return info_any_match();
  case Node::PLUS:
    return plus_info(analyze(n->kids[0].get(), o), o);
  case Node::QUEST:
    return alt_info(analyze(n->kids[0].get(), o), info_empty(), o);
  case Node::REPEAT: {
    if (n->rmin == 0 && n->rmax == 0)
      return info_empty();
    if (n->rmin == 0)
      return info_any_match();
    // x{m,..}：把 x 精确展开 min(m,4) 次再按 plus 处理（保守且可捕获跨拷贝
    // gram）
    Info acc = info_empty();
    int reps = min(n->rmin, 4);
    for (int k = 0; k < reps; k++)
      acc = concat_info(move(acc), analyze(n->kids[0].get(), o), o);
    if (n->rmax == n->rmin && n->rmin <= 4)
      return acc;
    return plus_info(move(acc), o);
  }
  }
  return info_any_match();
}
// 正则 → gram 查询；返回 false 表示无法解析（应保守回退全表扫描）
static bool compile_regex_to_q(const string &re, const Opts &o, Q &out,
                               string &err) {
  Parser p(re);
  NP root = p.parse();
  if (!p.ok) {
    err = p.err;
    return false;
  }
  Info info = analyze(root.get(), o);
  Q m = info.match;
  if (info.has_exact)
    m = q_and(move(m), q_of_set(info.exact, o));
  else {
    m = q_and(move(m), q_of_set(info.prefix, o));
    m = q_and(move(m), q_of_set(info.suffix, o));
  }
  out = move(m);
  return true;
}

// ------------------------------------------------------------------ 语料
struct Corpus {
  vector<string> rows;
  size_t bytes = 0;
};
static bool extract_json_field(const string &line, const string &field,
                               string &out) {
  string key = "\"" + field + "\"";
  size_t k = line.find(key);
  if (k == string::npos)
    return false;
  size_t c = line.find(':', k + key.size());
  if (c == string::npos)
    return false;
  size_t q = line.find('"', c);
  if (q == string::npos)
    return false;
  string v;
  for (size_t i = q + 1; i < line.size(); i++) {
    char ch = line[i];
    if (ch == '\\' && i + 1 < line.size()) {
      char e = line[++i];
      v.push_back(e == 'n' ? '\n' : e == 't' ? '\t' : e);
      continue;
    }
    if (ch == '"')
      break;
    v.push_back(ch);
  }
  out = move(v);
  return true;
}
static Corpus load_corpus(const Opts &o) {
  Corpus c;
  ifstream in(o.corpus);
  if (!in) {
    fprintf(stderr, "cannot open corpus %s\n", o.corpus.c_str());
    exit(1);
  }
  string line;
  while (getline(in, line)) {
    if (!o.field.empty()) {
      string v;
      if (!extract_json_field(line, o.field, v))
        continue;
      line = move(v);
    }
    c.bytes += line.size();
    c.rows.push_back(move(line));
    if (o.max_rows && c.rows.size() >= o.max_rows)
      break;
  }
  return c;
}

// ------------------------------------------------------------------ 体积口径
// FOR-128：每 128 个 delta 一块，块内按最大 delta 位宽打包，1 字节块头（近似 V4
// 的 PFOR 窗口，不含 patch 优化）
static uint64_t for128_bits(const vector<uint32_t> &ids) {
  uint64_t bits = 0;
  uint32_t prev = 0;
  for (size_t i = 0; i < ids.size(); i += 128) {
    size_t e = min(ids.size(), i + 128);
    uint32_t mx = 0;
    for (size_t j = i; j < e; j++) {
      uint32_t d = ids[j] - prev;
      prev = ids[j];
      mx = max(mx, d);
    }
    int w = mx == 0 ? 0 : (32 - __builtin_clz(mx));
    bits += 8 + (uint64_t)w * (e - i);
  }
  return bits;
}
static uint64_t vbyte_bytes(const vector<uint32_t> &ids) {
  uint64_t b = 0;
  uint32_t prev = 0;
  for (auto id : ids) {
    uint32_t d = id - prev;
    prev = id;
    b += d < 128          ? 1
         : d < 16384      ? 2
         : d < (1u << 21) ? 3
         : d < (1u << 28) ? 4
                          : 5;
  }
  return b;
}
static uint64_t roaring_bytes(const vector<uint32_t> &ids) {
  Roaring r;
  r.addMany(ids.size(), ids.data());
  r.runOptimize();
  return r.getSizeInBytes(true);
}

// ------------------------------------------------------------------
// 布隆过滤器（块级）
static inline uint64_t mix64(uint64_t x) {
  x ^= x >> 30;
  x *= 0xbf58476d1ce4e5b9ULL;
  x ^= x >> 27;
  x *= 0x94d049bb133111ebULL;
  x ^= x >> 31;
  return x;
}
struct Bloom {
  vector<uint64_t> bits;
  uint64_t m = 0;
  int k = 1;
  void init(uint64_t nbits, int kk) {
    m = max<uint64_t>(64, nbits);
    bits.assign((m + 63) / 64, 0);
    k = kk;
  }
  void add(uint64_t key) {
    uint64_t h1 = mix64(key), h2 = mix64(key ^ 0x9E3779B97F4A7C15ULL) | 1;
    for (int i = 0; i < k; i++) {
      uint64_t b = (h1 + i * h2) % m;
      bits[b >> 6] |= 1ULL << (b & 63);
    }
  }
  bool test(uint64_t key) const {
    uint64_t h1 = mix64(key), h2 = mix64(key ^ 0x9E3779B97F4A7C15ULL) | 1;
    for (int i = 0; i < k; i++) {
      uint64_t b = (h1 + i * h2) % m;
      if (!(bits[b >> 6] & (1ULL << (b & 63))))
        return false;
    }
    return true;
  }
  uint64_t bytes() const { return bits.size() * 8; }
};

// ------------------------------------------------------------------ 索引
struct Index {
  uint32_t N = 0;
  unordered_map<uint64_t, vector<uint32_t>> post; // 行级倒排（有序 docid）
  unordered_map<uint64_t, Roaring> row_bm;        // 行级 bitmap
  map<pair<uint32_t, uint64_t>, Roaring> blk_cache; // (B, gram) → 块 bitmap
  map<pair<uint32_t, int>, vector<Bloom>> blooms;   // (B, bpp) → 每块布隆
  vector<uint32_t>
      blk_distinct[8]; // 每 B 每块 distinct gram 数（顺序同 blocks）
  unordered_set<uint64_t> dropped; // 因 df 过高被丢弃的 gram
  Roaring universe;
};
static void build_index(const Corpus &c, const Opts &o, Index &ix) {
  ix.N = c.rows.size();
  vector<uint64_t> g;
  ix.post.reserve(1 << 17);
  for (uint32_t d = 0; d < ix.N; d++) {
    grams_of(c.rows[d], o, g);
    dedupe(g);
    for (auto k : g)
      ix.post[k].push_back(d);
  }
  ix.universe.addRange(0, ix.N);
  if (o.drop_df > 0) {
    for (auto &kv : ix.post)
      if ((double)kv.second.size() / ix.N > o.drop_df)
        ix.dropped.insert(kv.first);
  }
  for (auto &kv : ix.post) {
    if (ix.dropped.count(kv.first))
      continue;
    Roaring r;
    r.addMany(kv.second.size(), kv.second.data());
    r.runOptimize();
    ix.row_bm.emplace(kv.first, move(r));
  }
  // 每块 distinct gram + 布隆
  for (size_t bi = 0; bi < o.blocks.size(); bi++) {
    uint32_t B = o.blocks[bi];
    uint32_t nb = (ix.N + B - 1) / B;
    ix.blk_distinct[bi].assign(nb, 0);
    vector<vector<uint64_t>> per_block(nb);
    for (auto &kv : ix.post) {
      uint32_t last = UINT32_MAX;
      for (auto d : kv.second) {
        uint32_t b = d / B;
        if (b != last) {
          per_block[b].push_back(kv.first);
          last = b;
        }
      }
    }
    for (uint32_t b = 0; b < nb; b++)
      ix.blk_distinct[bi][b] = per_block[b].size();
    for (int bpp : o.bpp) {
      vector<Bloom> bf(nb);
      int k = max(1, (int)lround(bpp * 0.693));
      for (uint32_t b = 0; b < nb; b++) {
        bf[b].init((uint64_t)per_block[b].size() * bpp, k);
        for (auto key : per_block[b])
          if (!ix.dropped.count(key))
            bf[b].add(key);
      }
      ix.blooms[{B, bpp}] = move(bf);
    }
  }
}
static const Roaring *blk_bitmap(Index &ix, uint32_t B, uint64_t gram) {
  auto key = make_pair(B, gram);
  auto it = ix.blk_cache.find(key);
  if (it != ix.blk_cache.end())
    return &it->second;
  auto pit = ix.post.find(gram);
  Roaring r;
  if (pit != ix.post.end()) {
    vector<uint32_t> b;
    uint32_t last = UINT32_MAX;
    for (auto d : pit->second) {
      uint32_t x = d / B;
      if (x != last) {
        b.push_back(x);
        last = x;
      }
    }
    r.addMany(b.size(), b.data());
  }
  r.runOptimize();
  return &ix.blk_cache.emplace(key, move(r)).first->second;
}

struct SizeReport {
  uint64_t distinct = 0, postings = 0, roaring = 0, for128_bytes = 0, vbyte = 0,
           dict = 0;
  // 两层口径：df ≤ 2 的稀有 gram 不存词典文本，只存 4B 指纹 + 每 docid 3B；其余走词典 + FOR posting
  uint64_t tier_fp = 0, tier_dict = 0, tier_post = 0;
};
static SizeReport row_sizes(const Index &ix, const Opts &o) {
  SizeReport s;
  for (auto &kv : ix.post) {
    if (ix.dropped.count(kv.first))
      continue;
    s.distinct++;
    s.postings += kv.second.size();
    s.roaring += roaring_bytes(kv.second);
    s.for128_bytes += (for128_bits(kv.second) + 7) / 8;
    s.vbyte += vbyte_bytes(kv.second);
    if (kv.second.size() <= 2) s.tier_fp += 4 + 3 * kv.second.size();
    else {
      s.tier_post += (for128_bits(kv.second) + 7) / 8;
      size_t glen = o.n * (o.cp ? 3 : 1);
      if (o.cdc) { auto it = g_gram_text.find(kv.first); if (it != g_gram_text.end()) glen = it->second.size(); }
      s.tier_dict += glen + 6;
    }
  }
  s.dict = s.distinct * (o.n * (o.cp ? 3 : 1) + 6);
  if (o.cdc) { s.dict = 0; for (auto &kv : ix.post) if (!ix.dropped.count(kv.first)) { auto it = g_gram_text.find(kv.first); s.dict += (it == g_gram_text.end() ? o.n : (int)it->second.size()) + 6; } }
  return s;
}
static SizeReport blk_sizes(Index &ix, uint32_t B, const Opts &o) {
  SizeReport s;
  for (auto &kv : ix.post) {
    if (ix.dropped.count(kv.first))
      continue;
    vector<uint32_t> b;
    uint32_t last = UINT32_MAX;
    for (auto d : kv.second) {
      uint32_t x = d / B;
      if (x != last) {
        b.push_back(x);
        last = x;
      }
    }
    s.distinct++;
    s.postings += b.size();
    s.roaring += roaring_bytes(b);
    s.for128_bytes += (for128_bits(b) + 7) / 8;
    s.vbyte += vbyte_bytes(b);
  }
  s.dict = s.distinct * (o.n * (o.cp ? 3 : 1) + 6);
  if (o.cdc) { s.dict = 0; for (auto &kv : ix.post) if (!ix.dropped.count(kv.first)) { auto it = g_gram_text.find(kv.first); s.dict += (it == g_gram_text.end() ? o.n : (int)it->second.size()) + 6; } }
  return s;
}

// ------------------------------------------------------------------ 查询求值
static Roaring eval_rows(const Q &q, Index &ix, bool &unindexable) {
  if (q.op == Q::ALL) {
    unindexable = true;
    return ix.universe;
  }
  if (q.op == Q::NONE)
    return Roaring();
  if (q.op == Q::AND) {
    // 先按 df 升序求交
    vector<pair<uint64_t, uint64_t>> order; // (card, gram)
    for (auto g : q.grams) {
      if (ix.dropped.count(g))
        continue; // 被丢弃的 gram 视为 ALL
      auto it = ix.row_bm.find(g);
      order.push_back(
          {it == ix.row_bm.end() ? 0 : it->second.cardinality(), g});
    }
    sort(order.begin(), order.end());
    bool have = false;
    Roaring r;
    for (auto &[card, g] : order) {
      auto it = ix.row_bm.find(g);
      Roaring cur = it == ix.row_bm.end() ? Roaring() : it->second;
      if (!have) {
        r = move(cur);
        have = true;
      } else
        r &= cur;
      if (r.isEmpty())
        return r;
    }
    for (auto &s : q.subs) {
      bool ua = false;
      Roaring cur = eval_rows(s, ix, ua);
      if (ua)
        continue;
      if (!have) {
        r = move(cur);
        have = true;
      } else
        r &= cur;
    }
    if (!have) {
      unindexable = true;
      return ix.universe;
    }
    return r;
  }
  Roaring r;
  for (auto g : q.grams) {
    if (ix.dropped.count(g)) {
      unindexable = true;
      return ix.universe;
    }
    auto it = ix.row_bm.find(g);
    if (it != ix.row_bm.end())
      r |= it->second;
  }
  for (auto &s : q.subs) {
    bool ua = false;
    Roaring cur = eval_rows(s, ix, ua);
    if (ua) {
      unindexable = true;
      return ix.universe;
    }
    r |= cur;
  }
  return r;
}
static Roaring eval_blocks(const Q &q, Index &ix, uint32_t B,
                           bool &unindexable) {
  uint32_t nb = (ix.N + B - 1) / B;
  Roaring all;
  all.addRange(0, nb);
  if (q.op == Q::ALL) {
    unindexable = true;
    return all;
  }
  if (q.op == Q::NONE)
    return Roaring();
  if (q.op == Q::AND) {
    bool have = false;
    Roaring r;
    vector<pair<uint64_t, uint64_t>> order;
    for (auto g : q.grams) {
      if (ix.dropped.count(g))
        continue;
      order.push_back({blk_bitmap(ix, B, g)->cardinality(), g});
    }
    sort(order.begin(), order.end());
    for (auto &[card, g] : order) {
      const Roaring *cur = blk_bitmap(ix, B, g);
      if (!have) {
        r = *cur;
        have = true;
      } else
        r &= *cur;
      if (r.isEmpty())
        return r;
    }
    for (auto &s : q.subs) {
      bool ua = false;
      Roaring cur = eval_blocks(s, ix, B, ua);
      if (ua)
        continue;
      if (!have) {
        r = move(cur);
        have = true;
      } else
        r &= cur;
    }
    if (!have) {
      unindexable = true;
      return all;
    }
    return r;
  }
  Roaring r;
  for (auto g : q.grams) {
    if (ix.dropped.count(g)) {
      unindexable = true;
      return all;
    }
    r |= *blk_bitmap(ix, B, g);
  }
  for (auto &s : q.subs) {
    bool ua = false;
    Roaring cur = eval_blocks(s, ix, B, ua);
    if (ua) {
      unindexable = true;
      return all;
    }
    r |= cur;
  }
  return r;
}
static bool eval_bloom_block(const Q &q, const Bloom &bf, const Index &ix) {
  if (q.op == Q::ALL)
    return true;
  if (q.op == Q::NONE)
    return false;
  if (q.op == Q::AND) {
    for (auto g : q.grams) {
      if (ix.dropped.count(g))
        continue;
      if (!bf.test(g))
        return false;
    }
    for (auto &s : q.subs)
      if (!eval_bloom_block(s, bf, ix))
        return false;
    return true;
  }
  for (auto g : q.grams) {
    if (ix.dropped.count(g))
      return true;
    if (bf.test(g))
      return true;
  }
  for (auto &s : q.subs)
    if (eval_bloom_block(s, bf, ix))
      return true;
  return false;
}

// ------------------------------------------------------------------ 计时
static double now_ms() {
  return chrono::duration<double, milli>(
             chrono::steady_clock::now().time_since_epoch())
      .count();
}
static int hs_cb(unsigned, unsigned long long, unsigned long long, unsigned,
                 void *ctx) {
  *(bool *)ctx = true;
  return 1;
}

struct QueryResult {
  string re;
  Q q;
  bool parsed = true;
  string qstr;
  int leaves = 0;
  uint64_t truth = 0, row_cand = 0;
  bool row_unidx = false;
  map<uint32_t, uint64_t> blk_cand, blk_rows;                // 倒排块级
  map<pair<uint32_t, int>, uint64_t> bloom_cand, bloom_rows; // 布隆块级
  double t_re2_full = 0, t_hs_full = 0, t_idx_row = 0, t_verify_row = 0;
  map<uint32_t, double> t_verify_blk;
  map<pair<uint32_t, int>, double> t_verify_bloom;
  map<uint32_t, double> t_idx_blk;
  bool superset_ok = true;
  uint64_t io_bytes_row = 0; // 行级：查询触达的 posting 字节（Roaring 口径）
  bool is_literal = false;   // 纯字面量正则（Doris 现有快路径 = memmem）
  string literal;
  double t_lit_full = -1, t_ver_fast = 0; // memmem 全扫；真实口径复验（字面量 memmem / 否则 hs）
};
static bool node_pure_literal(const Node *n, string &out) {
  if (n->t == Node::LIT) { out += n->lit; return true; }
  if (n->t == Node::CAT) { for (auto &k : n->kids) if (!node_pure_literal(k.get(), out)) return false; return true; }
  return false;
}
static bool pure_literal(const string &re, string &lit) {
  Parser p(re); NP root = p.parse();
  if (!p.ok || p.icase) return false;
  lit.clear();
  return node_pure_literal(root.get(), lit) && !lit.empty();
}

static void run_query(const string &re, const Corpus &c, Index &ix,
                      const Opts &o, QueryResult &qr) {
  qr.re = re;
  string err;
  if (!compile_regex_to_q(re, o, qr.q, err)) {
    qr.parsed = false;
    qr.qstr = "PARSE-ERR:" + err;
    qr.q = q_all();
  } else {
    qr.qstr = q_str(qr.q, o);
    qr.leaves = q_leaf_count(qr.q);
  }

  RE2 rx(re, RE2::Quiet);
  if (!rx.ok()) {
    qr.qstr = "RE2-ERR";
    qr.parsed = false;
    return;
  }
  // 真值 + 无索引 re2 全扫
  vector<uint8_t> truth(ix.N, 0);
  double t0 = now_ms();
  for (uint32_t d = 0; d < ix.N; d++)
    if (RE2::PartialMatch(re2::StringPiece(c.rows[d].data(), c.rows[d].size()),
                          rx)) {
      truth[d] = 1;
      qr.truth++;
    }
  qr.t_re2_full = now_ms() - t0;
  // hyperscan 全扫（db/scratch 保留到复验之后再释放）
  hs_database_t *db = nullptr;
  hs_compile_error_t *herr = nullptr;
  hs_scratch_t *sc = nullptr;
  if (hs_compile(re.c_str(), HS_FLAG_SINGLEMATCH | HS_FLAG_UTF8, HS_MODE_BLOCK, nullptr, &db, &herr) == HS_SUCCESS) {
    hs_alloc_scratch(db, &sc);
    uint64_t hits = 0;
    t0 = now_ms();
    for (uint32_t d = 0; d < ix.N; d++) { bool m = false; hs_scan(db, c.rows[d].data(), c.rows[d].size(), 0, sc, hs_cb, &m); hits += m; }
    qr.t_hs_full = now_ms() - t0;
    if (hits != qr.truth && o.verbose) fprintf(stderr, "  [warn] hs hits %llu != re2 %llu for %s\n", (unsigned long long)hits, (unsigned long long)qr.truth, re.c_str());
  } else { qr.t_hs_full = -1; if (herr) hs_free_compile_error(herr); db = nullptr; }
  // 纯字面量：memmem 全扫基线（Doris 现有 REGEXP 对简单模式已走此快路径）
  qr.is_literal = pure_literal(re, qr.literal);
  if (qr.is_literal) {
    uint64_t hits = 0;
    t0 = now_ms();
    for (uint32_t d = 0; d < ix.N; d++) hits += memmem(c.rows[d].data(), c.rows[d].size(), qr.literal.data(), qr.literal.size()) != nullptr;
    qr.t_lit_full = now_ms() - t0;
    if (hits != qr.truth && o.verbose) fprintf(stderr, "  [warn] memmem hits %llu != re2 %llu\n", (unsigned long long)hits, (unsigned long long)qr.truth);
  }
  // 行级索引
  t0 = now_ms();
  Roaring rows = eval_rows(qr.q, ix, qr.row_unidx);
  qr.t_idx_row = now_ms() - t0;
  qr.row_cand = rows.cardinality();
  {
    // 触达字节：查询里所有 gram 的 Roaring 大小
    function<void(const Q &)> walk = [&](const Q &q) {
      for (auto g : q.grams) {
        auto it = ix.post.find(g);
        if (it != ix.post.end() && !ix.dropped.count(g))
          qr.io_bytes_row += roaring_bytes(it->second);
      }
      for (auto &s : q.subs)
        walk(s);
    };
    walk(qr.q);
  }
  // 复验（行级候选）
  t0 = now_ms();
  uint64_t verified = 0;
  for (auto it = rows.begin(); it != rows.end(); ++it) {
    uint32_t d = *it;
    if (RE2::PartialMatch(re2::StringPiece(c.rows[d].data(), c.rows[d].size()),
                          rx))
      verified++;
  }
  qr.t_verify_row = now_ms() - t0;
  {  // 真实口径复验：字面量 memmem，否则 hyperscan，否则 re2
    t0 = now_ms();
    uint64_t vf = 0;
    for (auto it = rows.begin(); it != rows.end(); ++it) {
      uint32_t d = *it;
      if (qr.is_literal) vf += memmem(c.rows[d].data(), c.rows[d].size(), qr.literal.data(), qr.literal.size()) != nullptr;
      else if (db) { bool m = false; hs_scan(db, c.rows[d].data(), c.rows[d].size(), 0, sc, hs_cb, &m); vf += m; }
      else vf += RE2::PartialMatch(re2::StringPiece(c.rows[d].data(), c.rows[d].size()), rx);
    }
    qr.t_ver_fast = now_ms() - t0;
    if (vf != qr.truth) qr.superset_ok = false;
  }
  // 超集断言
  for (uint32_t d = 0; d < ix.N; d++)
    if (truth[d] && !rows.contains(d)) {
      qr.superset_ok = false;
      break;
    }
  if (verified != qr.truth)
    qr.superset_ok = false;
  // 块级倒排
  for (uint32_t B : o.blocks) {
    bool ua = false;
    t0 = now_ms();
    Roaring bl = eval_blocks(qr.q, ix, B, ua);
    qr.t_idx_blk[B] = now_ms() - t0;
    qr.blk_cand[B] = bl.cardinality();
    uint64_t nrows = 0, ver = 0;
    t0 = now_ms();
    for (auto it = bl.begin(); it != bl.end(); ++it) {
      uint32_t b = *it;
      uint32_t e = min(ix.N, (b + 1) * B);
      for (uint32_t d = b * B; d < e; d++) {
        nrows++;
        if (RE2::PartialMatch(
                re2::StringPiece(c.rows[d].data(), c.rows[d].size()), rx))
          ver++;
      }
    }
    qr.t_verify_blk[B] = now_ms() - t0;
    qr.blk_rows[B] = nrows;
    if (ver != qr.truth)
      qr.superset_ok = false;
    // 布隆块级
    for (int bpp : o.bpp) {
      auto &bfs = ix.blooms[{B, bpp}];
      uint64_t cand = 0, nr = 0, v2 = 0;
      t0 = now_ms();
      for (uint32_t b = 0; b < bfs.size(); b++) {
        if (!eval_bloom_block(qr.q, bfs[b], ix))
          continue;
        cand++;
        uint32_t e = min(ix.N, (b + 1) * B);
        for (uint32_t d = b * B; d < e; d++) {
          nr++;
          if (RE2::PartialMatch(
                  re2::StringPiece(c.rows[d].data(), c.rows[d].size()), rx))
            v2++;
        }
      }
      qr.t_verify_bloom[{B, bpp}] = now_ms() - t0;
      qr.bloom_cand[{B, bpp}] = cand;
      qr.bloom_rows[{B, bpp}] = nr;
      if (v2 != qr.truth)
        qr.superset_ok = false;
    }
  }
  if (sc) hs_free_scratch(sc);
  if (db) hs_free_database(db);
}

static double median(vector<double> v) {
  if (v.empty())
    return 0;
  sort(v.begin(), v.end());
  size_t n = v.size();
  return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
}
static double pct(double a, double b) { return b == 0 ? 0 : 100.0 * a / b; }

int main(int argc, char **argv) {
  Opts o;
  for (int i = 1; i < argc; i++) {
    string a = argv[i];
    auto val = [&](const char *name) {
      if (a == name && i + 1 < argc)
        return string(argv[++i]);
      return string();
    };
    if (a == "--corpus")
      o.corpus = argv[++i];
    else if (a == "--queries")
      o.queries = argv[++i];
    else if (a == "--field")
      o.field = argv[++i];
    else if (a == "--n")
      o.n = atoi(argv[++i]);
    else if (a == "--cp")
      o.cp = true;
    else if (a == "--lower")
      o.lower = true;
    else if (a == "--max_rows")
      o.max_rows = atoll(argv[++i]);
    else if (a == "--drop_df")
      o.drop_df = atof(argv[++i]);
    else if (a == "--blocks") {
      o.blocks.clear();
      string s = argv[++i];
      stringstream ss(s);
      string t;
      while (getline(ss, t, ','))
        o.blocks.push_back(atoi(t.c_str()));
    } else if (a == "--bpp") {
      o.bpp.clear();
      string s = argv[++i];
      stringstream ss(s);
      string t;
      while (getline(ss, t, ','))
        o.bpp.push_back(atoi(t.c_str()));
    } else if (a == "--verbose")
      o.verbose = true;
    else if (a == "--cdc") o.cdc = true;
    else if (a == "--p") o.p = atof(argv[++i]);
    else if (a == "--maxlen") o.maxlen = atoi(argv[++i]);
    else if (a == "--explain") { // 只打印正则的 gram 查询
      string re = argv[++i];
      Q q;
      string err;
      if (!compile_regex_to_q(re, o, q, err))
        printf("PARSE-ERR %s\n", err.c_str());
      else
        printf("%s\n", q_str(q, o).c_str());
      return 0;
    }
    (void)val;
  }
  if (getenv("BENCH_EXTRACT")) {  // 只测 gram 提取吞吐：对全部行跑 grams_of 3 轮，丢弃结果
    Corpus cb = load_corpus(o);
    vector<uint64_t> g; uint64_t total = 0; double best = 1e18;
    for (int rep = 0; rep < 3; rep++) {
      double t0 = now_ms(); total = 0;
      for (auto &row : cb.rows) { grams_of(row, o, g); total += g.size(); }
      best = min(best, now_ms() - t0);
    }
    printf("extract: rows=%zu bytes=%zu grams=%llu grams/row=%.1f best=%.1f ms => %.1f MB/s, %.0f ns/row, %.1f ns/gram\n",
           cb.rows.size(), cb.bytes, (unsigned long long)total, (double)total / cb.rows.size(), best, cb.bytes / 1048576.0 / (best / 1000.0), best * 1e6 / cb.rows.size(), best * 1e6 / max<uint64_t>(1, total));
    return 0;
  }
  if (o.corpus.empty()) {
    fprintf(stderr, "usage: --corpus F [--queries Q] [--field name] [--n 3] "
                    "[--cp] [--lower] [--max_rows N] [--blocks 1024,4096] "
                    "[--bpp 8,12] [--drop_df 0.5]\n");
    return 1;
  }
  if (o.cp && o.n > 3) {
    fprintf(stderr, "cp mode supports n<=3\n");
    return 1;
  }

  double t0 = now_ms();
  Corpus c = load_corpus(o);
  fprintf(stderr, "[load] rows=%zu bytes=%zu (%.1f ms)\n", c.rows.size(),
          c.bytes, now_ms() - t0);
  Index ix;
  t0 = now_ms();
  build_index(c, o, ix);
  fprintf(stderr, "[build] distinct=%zu dropped=%zu (%.1f ms)\n",
          ix.post.size(), ix.dropped.size(), now_ms() - t0);

  printf("# corpus=%s field=%s rows=%u data_bytes=%zu avg_row=%.1f n=%d "
         "mode=%s lower=%d drop_df=%.2f\n",
         o.corpus.c_str(), o.field.c_str(), ix.N, c.bytes,
         (double)c.bytes / max<uint32_t>(1, ix.N), o.n,
         o.cdc ? "cdc" : (o.cp ? "codepoint" : "byte"), (int)o.lower, o.drop_df);
  if (o.cdc) printf("# cdc: p=%.3f maxlen=%d min_len(n)=%d\n", o.p, o.maxlen, o.n);

  // df 分布
  {
    vector<uint64_t> dfs;
    uint64_t total = 0, p10 = 0, p25 = 0, p50 = 0;
    for (auto &kv : ix.post) {
      dfs.push_back(kv.second.size());
      total += kv.second.size();
      double f = (double)kv.second.size() / ix.N;
      if (f > 0.1)
        p10 += kv.second.size();
      if (f > 0.25)
        p25 += kv.second.size();
      if (f > 0.5)
        p50 += kv.second.size();
    }
    sort(dfs.begin(), dfs.end());
    auto at = [&](double q) {
      return dfs.empty() ? 0ULL
                         : dfs[min(dfs.size() - 1, (size_t)(q * dfs.size()))];
    };
    printf("## gram-df: distinct=%zu postings=%llu postings_per_byte=%.3f "
           "df_p50=%llu df_p90=%llu df_p99=%llu df_max=%llu | posting-share of "
           "grams with df/N>0.1: %.1f%%, >0.25: %.1f%%, >0.5: %.1f%%\n",
           ix.post.size(), (unsigned long long)total,
           (double)total / max<size_t>(1, c.bytes), (unsigned long long)at(0.5),
           (unsigned long long)at(0.9), (unsigned long long)at(0.99),
           (unsigned long long)(dfs.empty() ? 0 : dfs.back()), pct(p10, total),
           pct(p25, total), pct(p50, total));
  }
  // 体积
  {  // df 分桶：posting 质量份额与 FOR-128 位成本（行级）
    const double edges[] = {0.001, 0.01, 0.05, 0.1, 0.25, 0.5, 1.01};
    const char *names[] = {"<=0.1%", "0.1-1%", "1-5%", "5-10%", "10-25%", "25-50%", ">50%"};
    uint64_t mass[7] = {0}, bits[7] = {0}, cnt[7] = {0}, total = 0;
    for (auto &kv : ix.post) {
      double f = (double)kv.second.size() / ix.N; int b = 0; while (f > edges[b]) b++;
      mass[b] += kv.second.size(); bits[b] += for128_bits(kv.second); cnt[b]++; total += kv.second.size();
    }
    printf("## df-buckets (row-level, FOR-128):");
    for (int b = 0; b < 7; b++) printf(" %s: grams=%llu mass=%.1f%% bytes=%.2f%%data bits/post=%.1f |", names[b], (unsigned long long)cnt[b], pct(mass[b], total), pct(bits[b] / 8.0, c.bytes), mass[b] ? (double)bits[b] / mass[b] : 0.0);
    printf("\n");
  }
  SizeReport rs = row_sizes(ix, o);
  printf("## size row-level: distinct=%llu postings=%llu roaring=%.2f%% "
         "for128=%.2f%% vbyte=%.2f%% dict=%.2f%% (bits/posting: roaring %.2f, "
         "for128 %.2f)\n",
         (unsigned long long)rs.distinct, (unsigned long long)rs.postings,
         pct(rs.roaring, c.bytes), pct(rs.for128_bytes, c.bytes),
         pct(rs.vbyte, c.bytes), pct(rs.dict, c.bytes),
         8.0 * rs.roaring / max<uint64_t>(1, rs.postings),
         8.0 * rs.for128_bytes / max<uint64_t>(1, rs.postings));
  printf("## size two-tier(R=2): fp_table=%.2f%% dict=%.2f%% postings=%.2f%% total=%.2f%%\n", pct(rs.tier_fp, c.bytes), pct(rs.tier_dict, c.bytes), pct(rs.tier_post, c.bytes), pct(rs.tier_fp + rs.tier_dict + rs.tier_post, c.bytes));
  for (size_t bi = 0; bi < o.blocks.size(); bi++) {
    uint32_t B = o.blocks[bi];
    SizeReport bs = blk_sizes(ix, B, o);
    uint64_t sum_d = 0, mx = 0;
    for (auto d : ix.blk_distinct[bi]) {
      sum_d += d;
      mx = max<uint64_t>(mx, d);
    }
    string bloom_s;
    for (int bpp : o.bpp) {
      uint64_t bytes = 0;
      for (auto &bf : ix.blooms[{B, bpp}])
        bytes += bf.bytes();
      char buf[64];
      snprintf(buf, sizeof buf, " bloom%d=%.2f%%", bpp, pct(bytes, c.bytes));
      bloom_s += buf;
    }
    printf("## size block-level B=%u: nblocks=%zu distinct_per_block avg=%.0f "
           "max=%llu | inverted roaring=%.2f%% for128=%.2f%% vbyte=%.2f%% "
           "dict=%.2f%% |%s\n",
           B, ix.blk_distinct[bi].size(),
           (double)sum_d / max<size_t>(1, ix.blk_distinct[bi].size()),
           (unsigned long long)mx, pct(bs.roaring, c.bytes),
           pct(bs.for128_bytes, c.bytes), pct(bs.vbyte, c.bytes),
           pct(bs.dict, c.bytes), bloom_s.c_str());
  }
  if (o.queries.empty())
    return 0;

  // 查询
  ifstream qin(o.queries);
  string line;
  vector<QueryResult> results;
  printf("\n## queries (times in ms; speedup = t_re2_full / (t_idx + "
         "t_verify))\n");
  printf("%-44s %-8s %-9s %-9s %-7s %-7s %-7s %-7s %-7s | %s\n", "regex",
         "truth", "row_cand", "spd_row", "re2ms", "hsms", "idxms", "verms",
         "ok",
         "block-level: B: inv_cand/inv_rows/spd  bloom(bpp): cand/rows/spd");
  while (getline(qin, line)) {
    if (line.empty() || line[0] == '#')
      continue;
    QueryResult qr;
    run_query(line, c, ix, o, qr);
    double spd_row = qr.t_re2_full / max(1e-6, qr.t_idx_row + qr.t_verify_row);
    double base_real = qr.is_literal ? qr.t_lit_full : (qr.t_hs_full > 0 ? qr.t_hs_full : qr.t_re2_full);
    double spd_real = base_real / max(1e-6, qr.t_idx_row + qr.t_ver_fast);
    string blk;
    for (uint32_t B : o.blocks) {
      char buf[256];
      double spd =
          qr.t_re2_full / max(1e-6, qr.t_idx_blk[B] + qr.t_verify_blk[B]);
      snprintf(buf, sizeof buf, " B%u:%llu/%llu/%.1fx", B,
               (unsigned long long)qr.blk_cand[B],
               (unsigned long long)qr.blk_rows[B], spd);
      blk += buf;
      for (int bpp : o.bpp) {
        double s2 = qr.t_re2_full / max(1e-6, qr.t_verify_bloom[{B, bpp}]);
        snprintf(buf, sizeof buf, " bf%d:%llu/%llu/%.1fx", bpp,
                 (unsigned long long)qr.bloom_cand[{B, bpp}],
                 (unsigned long long)qr.bloom_rows[{B, bpp}], s2);
        blk += buf;
      }
    }
    printf(
        "%-44.44s %-8llu %-9llu %-9.1f %-7.1f %-7.1f %-7.2f %-7.1f %-7s |%s\n",
        qr.re.c_str(), (unsigned long long)qr.truth,
        (unsigned long long)qr.row_cand, spd_row, qr.t_re2_full, qr.t_hs_full,
        qr.t_idx_row, qr.t_verify_row, qr.superset_ok ? "ok" : "VIOLATION",
        blk.c_str());
    printf("    REAL: base=%s %.1fms  idx+fastverify=%.2f+%.1fms  spd_real=%.1fx\n", qr.is_literal ? "memmem" : (qr.t_hs_full > 0 ? "hs" : "re2"), base_real, qr.t_idx_row, qr.t_ver_fast, spd_real);
    printf("    gramq[%d]=%s  io_row=%.1fKB%s\n", qr.leaves, qr.qstr.c_str(),
           qr.io_bytes_row / 1024.0,
           qr.row_unidx ? "  [UNINDEXABLE->fullscan]" : "");
    results.push_back(move(qr));
  }
  // 汇总
  vector<double> s_row, s_hs;
  map<uint32_t, vector<double>> s_blk;
  map<pair<uint32_t, int>, vector<double>> s_bf;
  vector<double> s_real, s_real_idx;
  int indexable = 0;
  for (auto &qr : results) {
    if (!qr.row_unidx)
      indexable++;
    s_row.push_back(qr.t_re2_full / max(1e-6, qr.t_idx_row + qr.t_verify_row));
    { double base_real = qr.is_literal ? qr.t_lit_full : (qr.t_hs_full > 0 ? qr.t_hs_full : qr.t_re2_full);
      double sr = base_real / max(1e-6, qr.t_idx_row + qr.t_ver_fast);
      s_real.push_back(sr); if (!qr.row_unidx) s_real_idx.push_back(sr); }
    if (qr.t_hs_full > 0)
      s_hs.push_back(qr.t_re2_full / qr.t_hs_full);
    for (uint32_t B : o.blocks) {
      s_blk[B].push_back(qr.t_re2_full /
                         max(1e-6, qr.t_idx_blk[B] + qr.t_verify_blk[B]));
      for (int bpp : o.bpp)
        s_bf[{B, bpp}].push_back(qr.t_re2_full /
                                 max(1e-6, qr.t_verify_bloom[{B, bpp}]));
    }
  }
  printf("\n## summary: queries=%zu indexable=%d | median speedup: "
         "row-level=%.1fx hs-vs-re2(no index)=%.1fx",
         results.size(), indexable, median(s_row), median(s_hs));
  printf(" | REAL(memmem/hs baseline, fast verify): median all=%.1fx indexable-only=%.1fx", median(s_real), median(s_real_idx));
  for (uint32_t B : o.blocks) {
    printf(" | B=%u inverted=%.1fx", B, median(s_blk[B]));
    for (int bpp : o.bpp)
      printf(" bloom%d=%.1fx", bpp, median(s_bf[{B, bpp}]));
  }
  printf("\n");
  return 0;
}
