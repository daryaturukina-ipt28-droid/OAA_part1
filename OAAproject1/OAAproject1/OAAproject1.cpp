#include <algorithm>
#include <cctype>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#include <io.h>
#define IS_TTY() (_isatty(_fileno(stdin))!=0)
#else
#include <unistd.h>
#define IS_TTY() (isatty(STDIN_FILENO)!=0)
#endif

using namespace std;

struct CommandError : runtime_error {
    using runtime_error::runtime_error;
};


static size_t quoteLen(const string& s, size_t i) {
    if (i>=s.size()) return 0;
    if (s[i]=='"') return 1;
    if (i+2<s.size() && (unsigned char)s[i]==0xE2 && (unsigned char)s[i+1]==0x80 && ((unsigned char)s[i+2]==0x9C || (unsigned char)s[i+2]==0x9D))
        return 3;
    return 0;
}

static bool isWordChar(char c) {
    return (c>='a' && c<='z') || (c>='A' && c<='Z') || (c>='0' && c<='9') || c=='_';
}
static bool isAlpha(char c) { return (c>='a' && c<='z') || (c>='A' && c<='Z'); }
static bool isDigit(char c) { return c>='0' && c<='9'; }
static bool isSpace(char c) { return c==' ' || c=='\t' || c=='\r' || c=='\n' || c=='\f' || c=='\v'; }

static string toLower(string s) {
    for (auto& c : s) c=(char)tolower((unsigned char)c);
    return s;
}
static string toUpper(string s) {
    for (auto& c : s) c=(char)toupper((unsigned char)c);
    return s;
}


class Collection {
public:
    using Postings=map<int, vector<int>>;

    int insert(const string& text) {
        int docId=(int)docs_.size()+1;
        docs_.push_back(text);
        int pos=0;
        size_t i=0, n=text.size();
        while (i<n) {
            if (!isWordChar(text[i])) { ++i; continue; }
            size_t j=i;
            while (j<n && isWordChar(text[j])) ++j;
            ++pos;
            index_[toLower(text.substr(i, j-i))][docId].push_back(pos);
            i=j;
        }
        return docId;
    }

    const vector<string>& docs() const { return docs_; }

    vector<int> all() const {
        vector<int> r(docs_.size());
        for (size_t i=0; i<r.size(); ++i) r[i]=(int)i+1;
        return r;
    }

    vector<int> searchKeyword(const string& w) const {
        vector<int> r;
        auto it=index_.find(w);
        if (it!=index_.end())
            for (auto& kv : it->second) r.push_back(kv.first);
        return r;
    }

    vector<int> searchPrefix(const string& prefix) const {
        set<int> ids;
        for (auto it=index_.lower_bound(prefix);
            it!=index_.end() && it->first.compare(0, prefix.size(), prefix)==0; ++it)
            for (auto& kv : it->second) ids.insert(kv.first);
        return vector<int>(ids.begin(), ids.end());
    }

    vector<int> searchNear(const string& w1, const string& w2, long long n) const {
        vector<int> r;
        auto a=index_.find(w1), b=index_.find(w2);
        if (a==index_.end() || b==index_.end()) return r;
        const Postings& pa=a->second;
        const Postings& pb=b->second;
        const Postings& small=pa.size()<=pb.size() ? pa : pb;
        for (auto& kv : small) {
            int doc=kv.first;
            auto ia=pa.find(doc), ib=pb.find(doc);
            if (ia==pa.end() || ib==pb.end()) continue;
            const vector<int>& v2 = ib->second;
            for (int p : ia->second) {
                if (binary_search(v2.begin(), v2.end(), (long long)p+n) ||
                    binary_search(v2.begin(), v2.end(), (long long)p-n)) {
                    r.push_back(doc);
                    break;
                }
            }
        }
        return r;
    }

    string printIndex() const {
        if (index_.empty()) return "index is empty";
        ostringstream out;
        bool first=true;
        for (auto& [word, postings] : index_) {
            if (!first) out<<'\n';
            first=false;
            out<<'"'<<word<<"\":";
            for (auto& [doc, positions] : postings) {
                out<<"\n  d"<<doc<< " [";
                for (size_t k=0; k<positions.size(); ++k) {
                    if (k) out<<", ";
                    out<<positions[k];
                }
                out<<']';
            }
        }
        return out.str();
    }

private:
    vector<string> docs_;
    map<string, Postings> index_;
};


enum class Kind { String, Ident, Number, Sym };
struct Token {
    Kind kind;
    string value;
};

static vector<Token> tokenize(const string& s) {
    vector<Token> tokens;
    size_t i=0, n=s.size();
    while (true) {
        while (i<n && isSpace(s[i])) ++i;
        if (i>=n) return tokens;

        if (size_t ql=quoteLen(s, i)) {
            size_t start=i+ql, j=start;
            while (true) {
                if (j>=n || s[j]=='\n')
                    throw CommandError("Error, unterminated string "
                        "strings must be closed on the same line");
                if (size_t cl=quoteLen(s, j)) {
                    tokens.push_back({ Kind::String, s.substr(start, j-start) });
                    i=j+cl;
                    break;
                }
                ++j;
            }
        }
        else if (isAlpha(s[i])) {
            size_t j=i;
            while (j<n && isWordChar(s[j])) ++j;
            tokens.push_back({ Kind::Ident, s.substr(i, j-i) });
            i=j;
        }
        else if (isDigit(s[i])) {
            size_t j=i;
            while (j<n && isDigit(s[j])) ++j;
            tokens.push_back({ Kind::Number, s.substr(i, j-i) });
            i=j;
        }
        else if (s[i]=='*' || s[i]=='<' || s[i]=='>') {
            tokens.push_back({ Kind::Sym, string(1, s[i]) });
            ++i;
        }
        else {
            throw CommandError(string("Error, unexpected character '")+s[i]+"'");
        }
    }
}


class Database {
public:

    string execute(const string& text) {
        vector<Token> t=tokenize(text);
        if (t.empty()) return "";
        if (t[0].kind!=Kind::Ident)
            throw CommandError("Error, command must start with a keyword");
        string cmd=toUpper(t[0].value);
        vector<Token> args(t.begin()+1, t.end());
        if (cmd=="CREATE") return create(args);
        if (cmd=="INSERT") return insert(args);
        if (cmd=="PRINT_INDEX") return printIndex(args);
        if (cmd=="SEARCH") return search(args);
        throw CommandError("Error, unknown command"+t[0].value);
    }

private:
    map<string, Collection> collections_;

    static const string& expect(const vector<Token>& a, size_t i, Kind k, const char* what) {
        if (i>=a.size())
            throw CommandError(string("Error, expected ")+what+", got end of command");
        if (a[i].kind!=k)
            throw CommandError(string("Error, expected ")+what+", got "+a[i].value);
        return a[i].value;
    }
    static void expectEnd(const vector<Token>& a, size_t i) {
        if (i<a.size()) throw CommandError("Error, unexpected "+a[i].value);
    }
    Collection& get(const string& name) {
        auto it=collections_.find(name);
        if (it==collections_.end()) throw CommandError("Collection "+name+" does not exist");
        return it->second;
    }

    string create(const vector<Token>& a) {
        const string& name=expect(a, 0, Kind::Ident, "collection name");
        expectEnd(a, 1);
        if (collections_.count(name)) throw CommandError("Collection '"+name+"' already exists");
        collections_.emplace(name, Collection());
        return "Collection "+name+" has been created";
    }

    string insert(const vector<Token>& a) {
        const string& name=expect(a, 0, Kind::Ident, "collection name");
        const string& text=expect(a, 1, Kind::String, "document text in quotes");
        expectEnd(a, 2);
        int id=get(name).insert(text);
        return "Document d"+to_string(id)+" has been added to "+name;
    }

    string printIndex(const vector<Token>& a) {
        const string& name=expect(a, 0, Kind::Ident, "collection name");
        expectEnd(a, 1);
        return get(name).printIndex();
    }

    static string word(const string& s) {
        if (s.empty() || !all_of(s.begin(), s.end(), isWordChar))
            throw CommandError("Invalid search word \""+s+"\": only a-z, A-Z, 0-9 characters are allowed");
        return toLower(s);
    }

    vector<int> runQuery(Collection& c, const vector<Token>& a, size_t i) {
        string w1=word(expect(a, i, Kind::String, "search word in quotes"));
        ++i;
        if (i==a.size()) return c.searchKeyword(w1);
        if (a[i].kind==Kind::Sym && a[i].value=="*") {
            expectEnd(a, i + 1);
            return c.searchPrefix(w1);
        }
        if (a[i].kind==Kind::Sym && a[i].value=="<") {
            const string& num=expect(a, i+1, Kind::Number, "distance");
            expect(a, i+2, Kind::Sym, "'>'");
            if (a[i+2].value!=">")
                throw CommandError("Error, expected '>', got"+a[i+2].value);
            string w2=word(expect(a, i+3, Kind::String, "second search word in quotes"));
            expectEnd(a, i+4);
            long long n;
            try {
                n=stoll(num);
            }
            catch (const out_of_range&) {
                return {};  
            }
            return c.searchNear(w1, w2, n);
        }
        throw CommandError("Error, unexpected "+a[i].value+" in query");
    }

    string search(const vector<Token>& a) {
        const string& name=expect(a, 0, Kind::Ident, "collection name");
        Collection& c=get(name);
        vector<int> ids;
        if (a.size()==1) {
            ids=c.all();
        }
        else {
            if (a[1].kind!=Kind::Ident || toUpper(a[1].value)!="WHERE")
                throw CommandError("Error, expected WHERE, got "+a[1].value);
            ids=runQuery(c, a, 2);
        }
        if (ids.empty()) return "No documents found";
        ostringstream out;
        for (int id : ids) out << 'd' << id << ": " << c.docs()[id - 1] << '\n';
        out << "Found " << ids.size() << " document(s)";
        return out.str();
    }
};


int main() {
    ios::sync_with_stdio(false);
    const bool interactive=IS_TTY();
    Database db;
    string buf, line;

    auto hasContent=[](const string& s) {
        return any_of(s.begin(), s.end(), [](char c) { return !isSpace(c); });
        };
    auto run=[&](const string& cmd) {
        try {
            string result=db.execute(cmd);
            if (!result.empty()) cout<<result<<'\n';
        }
        catch (const CommandError& e) {
            cout<<"Error: "<<e.what()<<'\n';
        }
        };

    while (true) {
        if (interactive) {
            cout << (hasContent(buf) ? "... " : "> ")<<flush;
        }
        if (!getline(cin, line)) break;
        line.push_back('\n');

        bool inStr=false;
        for (size_t i=0; i<line.size();) {
            size_t ql=quoteLen(line, i);
            if (inStr) {
                if (ql) {
                    buf.append(line, i, ql);
                    inStr=false;
                    i+=ql;
                }
                else {
                    buf.push_back(line[i++]);
                }
                continue;
            }
            if (line[i]==';') {
                run(buf);
                buf.clear();
                break;
            }
            if (ql) {
                buf.append(line, i, ql);
                inStr=true;
                i+=ql;
            }
            else {
                buf.push_back(line[i++]);
            }
        }
    }
    if (hasContent(buf)) cout<<"Error, missing ; at the end\n";
    return 0;
}