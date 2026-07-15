#include "Json.h"
#include <cctype>
#include <charconv>
#include <optional>

namespace desktop_update_kit::json { namespace {
class Parser { const std::string& s_; size_t p_{}; std::string& e_;
 void ws(){ while(p_<s_.size() && std::isspace(static_cast<unsigned char>(s_[p_]))) ++p_; }
 bool take(char c){ ws(); if(p_<s_.size()&&s_[p_]==c){++p_;return true;} return false; }
 bool text(const char* t){ ws(); auto q=p_; while(*t && q<s_.size() && s_[q]==*t){++q;++t;} if(*t) return false; p_=q; return true; }
 bool str(std::string& out){ ws(); if(p_>=s_.size()||s_[p_++]!='\"') return false; while(p_<s_.size()){ char c=s_[p_++]; if(c=='\"') return true; if(c=='\\'){ if(p_>=s_.size()) break; c=s_[p_++]; switch(c){case '\"':out+='\"';break;case '\\':out+='\\';break;case '/':out+='/';break;case 'b':out+='\b';break;case 'f':out+='\f';break;case 'n':out+='\n';break;case 'r':out+='\r';break;case 't':out+='\t';break; default:e_="Unsupported JSON escape";return false;} } else out+=c; } e_="Unterminated JSON string"; return false; }
 bool value(Value& v){ ws(); if(p_>=s_.size()){e_="Unexpected end of JSON";return false;} if(s_[p_]=='\"'){std::string x;if(!str(x))return false;v.data=std::move(x);return true;} if(s_[p_]=='{')return obj(v); if(s_[p_]=='[')return arr(v); if(text("true")){v.data=true;return true;} if(text("false")){v.data=false;return true;} if(text("null")){v.data=nullptr;return true;} size_t begin=p_; if(s_[p_]=='-')++p_; while(p_<s_.size()&&std::isdigit(static_cast<unsigned char>(s_[p_])))++p_; if(p_<s_.size()&&s_[p_]=='.'){++p_;while(p_<s_.size()&&std::isdigit(static_cast<unsigned char>(s_[p_])))++p_;} double n{}; auto r=std::from_chars(s_.data()+begin,s_.data()+p_,n); if(r.ec==std::errc{}&&r.ptr==s_.data()+p_){v.data=n;return true;} e_="Invalid JSON value";return false; }
 bool obj(Value& v){ if(!take('{'))return false; Value::object o; ws(); if(take('}')){v.data=std::move(o);return true;} while(true){std::string k;if(!str(k)){e_="Expected object key";return false;}if(!take(':')){e_="Expected colon";return false;}Value x;if(!value(x))return false;o.emplace(std::move(k),std::move(x));if(take('}'))break;if(!take(',')){e_="Expected comma";return false;}}v.data=std::move(o);return true; }
 bool arr(Value& v){ if(!take('['))return false; Value::array a; ws(); if(take(']')){v.data=std::move(a);return true;} while(true){Value x;if(!value(x))return false;a.push_back(std::move(x));if(take(']'))break;if(!take(',')){e_="Expected comma";return false;}}v.data=std::move(a);return true; }
 public: Parser(const std::string&s,std::string&e):s_(s),e_(e){} bool run(Value&v){if(!value(v))return false;ws();if(p_!=s_.size()){e_="Trailing JSON content";return false;}return true;} };
}
bool parse(const std::string& text, Value& value, std::string& error){ error.clear(); return Parser(text,error).run(value); }
const Value* member(const Value::object& o,const char*n){auto i=o.find(n);return i==o.end()?nullptr:&i->second;} const std::string* string(const Value*v){return v?std::get_if<std::string>(&v->data):nullptr;} std::optional<double> number(const Value*v){if(!v)return{};if(auto n=std::get_if<double>(&v->data))return*n;return{};} std::optional<bool> boolean(const Value*v){if(!v)return{};if(auto b=std::get_if<bool>(&v->data))return*b;return{};} const Value::array* array(const Value*v){return v?std::get_if<Value::array>(&v->data):nullptr;} const Value::object* object(const Value*v){return v?std::get_if<Value::object>(&v->data):nullptr;}
}
