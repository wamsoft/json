/**
 * copyright (c) 2007 Go Watanabe
 */
#include "tp_stub.h"
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <algorithm>

#ifndef _WIN32
typedef unsigned long ULONG;
#endif

using namespace std;

#define UNICODE_BOM (0xfeff)

static bool TVPUtf16ToUtf8( std::string& out, const tjs_char *in ) {
	tjs_int len = TVPWideCharToUtf8String( in, NULL );
	if( len < 0 ) return false;
	char* buf = new char[len];
	if( buf ) {
		try {
			len = TVPWideCharToUtf8String( in, buf );
			if( len > 0 ) out.assign( buf, len );
			delete[] buf;
		} catch(...) {
			delete[] buf;
			throw;
		}
	}
	return len > 0;
}

static bool TVPUtf16ToUtf8( std::string& out, const tjs_string& in ) {
	return TVPUtf16ToUtf8(out, in.c_str());
}

// ----------------------------------------------------------------------

class IFileStorage  {

	iTJSBinaryStream *in;
	char buf[8192];
	ULONG pos;
	ULONG len;
	bool eofFlag;
	bool utf8;
	
public:
	IFileStorage(tTJSVariantString *filename, bool utf8) : utf8(utf8) {
		in = TVPCreateBinaryStreamForRead(filename, "");
		if(!in) {
			TVPThrowExceptionMessage((ttstr(TJS_W("cannot open : ")) + *filename).c_str());
		}
		pos = 0;
		len = 0;
		eofFlag = false;
	}

	~IFileStorage() {
		if (in) {
			in->Destruct();
			in = NULL;
		}
	}
	
	int getc() {
		if (pos < len) {
			return buf[pos++];
		} else {
			if (!in || eofFlag) {
				return EOF;
			} else {
				pos = 0;
				if ((len = in->Read(buf, sizeof buf)) > 0) {
					eofFlag = len < sizeof buf;
				} else {
					eofFlag = true;
					len = 0;
				}
				return getc();
			}
		}
	}

	void ungetc() {
		if (pos > 0) {
			pos--;
		}
	}

	bool eof() {
		return pos >= len && eofFlag;
	}

	/**
	 * 改行チェック
	 */
	bool endOfLine(int c) {
		bool eol = (c =='\r' || c == '\n');
		if (c == '\r'){
			c = getc();
			if (!eof() && c != '\n') {
				ungetc();
			}
		}
		return eol;
	}
	
	bool addNextLine(ttstr &str) {
		int c;
		string mbline;
		while ((c = getc()) != EOF && !endOfLine(c)) {
			mbline += c;
		}
		int l = (int)mbline.length();
		if (l > 0 || c != EOF) {
			if (utf8) {
				tjs_char *buf = new tjs_char[l + 1];
				l = TVPUtf8ToWideCharString(mbline.data(), buf);
				buf[l] = '\0';
				str += buf;
				delete[] buf;
			} else {
				str += tTJSString(mbline.c_str());
			}
			return true;
		} else {
			return false;
		}
	}
};


// -----------------------------------------------------------------
class JSONTextReadStream : public iTJSTextReadStream
{
public:
  IFileStorage *Storage;
  tjs_int codepage;
  ttstr buf;
  tjs_int pos;

  JSONTextReadStream(tTJSVariantString *filename, tjs_int codepage) {
    Storage = new IFileStorage(filename, codepage);
    pos = 0;
  }

  ~JSONTextReadStream(void) {
    delete Storage;
  }

  virtual tjs_uint TJS_INTF_METHOD Read(tTJSString & targ, tjs_uint size) {
    tjs_uint readSize = 0;
    while (readSize < size) {
      if (pos >= buf.length()) {
	buf.Clear();
	pos = 0;
	if (! Storage->addNextLine(buf))
	  break;
      }
      tjs_uint n = min(tjs_uint(buf.length() - pos), size - readSize);
      readSize += n;
      while (n > 0) {
	targ += buf[pos];
	pos++;
	n--;
      }
    }
    return readSize;
  }

  virtual void TJS_INTF_METHOD Destruct() {
    delete this;
  }
};

// -----------------------------------------------------------------

class IReader {
public:

	virtual int getc() = 0;
	virtual void ungetc() = 0;
	virtual void close() = 0;

	bool isError;

	/**
	 * コンストラクタ
	 */
	IReader() {
		isError = false;
	}

	virtual ~IReader() {};
	
	/**
	 * エラー処理
	 */
	void error(const tjs_char *msg) {
		isError = true;
		TVPAddLog(msg);
	}
	
	/**
	 * 行末まで読み飛ばす
	 */
    void toEOL() {
        int c;
        do {
            c = getc();
		} while (c != EOF && c != '\n' && c != '\r');
    }

    /**
     * 空白とコメントを除去して次の文字を返す
     */
	int next() {
        for (;;) {
			int c = getc();
			if (c == '#') {
				toEOL();
			} else if (c == '/') {
				switch (getc()) {
				case '/':
					toEOL();
					break;
                case '*':
					for (;;) {
						c = getc();
						if (c == EOF) {
							error(TJS_W("コメントが閉じていません"));
							return EOF;
                        }
                        if (c == '*') {
                            if (getc() == '/') {
                                break;
                            }
                            ungetc();
                        }
                    }
                    break;
                default:
                    ungetc();
                    return '/';
                }
			} else if (c != UNICODE_BOM && (c == EOF || c > ' ')) {
				return c;
			}
		}
	}

    /**
     * 指定された文字数分の文字列を取得
     * @param str 文字列の格納先
     * @param n 文字数
     */
    void next(ttstr &str, int n) {
		str = "";
		while (n > 0) {
			int c = getc();
			if (c == EOF) {
				break;
			}
			str += c;
			n--;
		}
	}

    void next(tjs_string &str, int n) {
		str = TJS_W("");
		while (n > 0) {
			int c = getc();
			if (c == EOF) {
				break;
			}
			str += c;
			n--;
		}
	}

    void parseObject(tTJSVariant &var) {

        // 辞書を生成
        iTJSDispatch2 *dict = TJSCreateDictionaryObject();
        var = tTJSVariant(dict, dict);
	dict->Release();

        int c;

        for (;;) {

            tTJSVariant key;

            c = next();
			if (c == EOF) {
				error(TJS_W("オブジェクトは '}' で終了する必要があります"));
				return;
			} else if (c == '}') {
				return;
			} else if (c == ',' || c == ';') {
				ungetc();
			} else {

				ungetc();
				parse(key);

				c = next();
				if (c == '=') {
					if (getc() != '>') {
						ungetc();
					}
				} else if (c != ':') {
					error(TJS_W("キーの後には ':' または '=' または '=>' が必要です"));
					return;
				}

				// メンバ登録
				tTJSVariant value;
				parse(value);
				
				dict->PropSet(TJS_MEMBERENSURE, key.GetString(), NULL, &value, dict);
			}
			
			switch (next()) {
			case ';':
			case ',':
				break;
			case '}':
				return;
			default:
				error(TJS_W(" ',' または ';' または '}' が必要です"));
				return;
			}
        }
    }

	void parseArray(tTJSVariant &var) {
        
        // 配列を生成
		iTJSDispatch2 *array = TJSCreateArrayObject();
		var = tTJSVariant(array, array);
		array->Release();

        tjs_int cnt = 0;

        for (;;) {
            int ch = next();
			switch (ch) {
			case EOF:
				error(TJS_W("配列は ']' で終了する必要があります"));
				return;
			case ']':
				return;
			case ',':
			case ';':
				{
					ungetc();
					// 空のカラムを登録
					tTJSVariant value;
					array->PropSetByNum(TJS_MEMBERENSURE, cnt++, &value, array);
				}
				break;
			default:
				ungetc();
				tTJSVariant value;
				parse(value);
				array->PropSetByNum(TJS_MEMBERENSURE, cnt++, &value, array);
            }

            switch (next()) {
            case ';':
            case ',':
                break;
            case ']':
                return;
            default:
				error(TJS_W(" ',' または ';' または ']' が必要です"));
                return;
            }
        }
    }

    /**
	 * クオート文字列のパース
     * @param quote クオート文字
     * @param var 格納先
     */
	void parseQuoteString(int quote, tTJSVariant &var) {
		int c;
		ttstr str;
		for (;;) {
			c = getc();
			switch (c) {
			case 0:
			case '\n':
			case '\r':
				error(TJS_W("文字列が終端していません"));
				return;
			case '\\':
				c = getc();
				switch (c) {
				case 'b':
					str += '\b';
					break;
				case 'f':
					str += '\f';
					break;
				case 't':
					str += '\t';
					break;
				case 'r':
					str += '\r';
					break;
				case 'n':
					str += '\n';
					break;
				case 'u':
					{
						tjs_string work;
						next(work, 4);
						std::string out;
						TVPUtf16ToUtf8(out, work);
						str += (tjs_char)std::stol(out.c_str(), NULL, 16);
					}
					break;
				case 'x' :
					{
						tjs_string work;
						next(work, 2);
						std::string out;
						TVPUtf16ToUtf8(out, work);
						str += (tjs_char)std::stol(out.c_str(), NULL, 16);
					};
					break;
				default:
					str += c;
				}
				break;
			default:
				if (c == quote) {
					var = str;
					return;
				}
				str += c;
			}
		}
	}

	/**
	 * 指定した文字が数値の１文字目の構成要素かどうか
	 */
	bool isNumberFirst(int ch) {
		return (ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == '+';
	}

	/**
	 * 指定した文字が数値の構成要素かどうか
	 */
	bool isNumber(int ch) {
		return (ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == '+' || ch == 'e' || ch == 'E';
	}

	/**
	 * 指定した文字が文字列の構成要素かどうか
	 */
	bool isString(int ch) {
		return ch > 0x80 || ch > ' ' && TJS_strchr(TJS_W(",:]}/\\\"[{;=#"), ch) == NULL;
	}

	/**
	 * パースの実行
	 * @param var 結果格納先
	 */
	void parse(tTJSVariant &var) {
		
		int ch = next();

		switch (ch) {
		case '"':
		case '\'':
			// クオート文字列
			parseQuoteString(ch, var);
			break;
		case '{':
			// オブジェクト
			parseObject(var);
			break;
		case '[':
			parseArray(var);
			break;
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9':
		case '.':
		case '-':
		case '+':
			{
				// 数値
				bool doubleValue = false;
				
				tjs_string s;
				while (isNumber(ch)) {
					if (ch == '.') {
						doubleValue = true; // ひどい処理だ（笑)
					}
					s += ch;
					ch = getc();
				}
				ungetc();

				std::string out;
				TVPUtf16ToUtf8(out, s);

				// 数値
				if (doubleValue) {
					double value = std::strtod(out.c_str(), NULL);
					var = value;
				} else {
					tjs_int64 value = std::strtoll(out.c_str(), NULL, 10);
					var = value;
				}
			}
			break;
		default:
			if (ch >= 'a' && ch <= 'z') {
			
				// 文字列を抽出
				ttstr s;
				while (ch >= 'a' && ch <= 'z') {
					s += ch;
					ch = getc();
				}
				ungetc();
				
				// 識別子
				if (s == TJS_W("true")) {
					var = true;
				} else if (s == TJS_W("false")) {
					var = false;
				} else if (s == TJS_W("null")) {
					var.Clear();
				} else if (s == TJS_W("void")) {
					var.Clear();
				} else {
					ttstr msg = TJS_W("不明なキーワードです:");
					msg += s;
					error(msg.c_str());
				}
			} else {
				ttstr msg = TJS_W("不明な文字です:");
				error(msg.c_str());
			}
		}
    }
};

class IFileReader : public IReader {

	/// 入力バッファ
	ttstr buf;
	/// 入力ストリーム
	iTJSTextReadStream *stream;
	
	ULONG pos;
	bool eofFlag;
	
public:
	IFileReader(tTJSVariantString *filename, tjs_int codepage) {
		stream = new JSONTextReadStream(filename, codepage);
		pos = 0;
		eofFlag = false;
	}

	virtual void close() {
		if (stream) {
			stream->Destruct();
			stream = NULL;
		}
	}
	
	~IFileReader() {
		close();
	}

	
	int getc() {
		if (pos < buf.length()) {
			return buf.c_str()[pos++];
		} else {
			if (!stream || eofFlag) {
				return EOF;
			} else {
				pos = 0;
				buf.Clear();
				eofFlag = stream->Read(buf, 1024) < 1024;
				return getc();
			}
		}
	}

	void ungetc() {
		if (pos > 0) {
			pos--;
		}
	}

};

class IStringReader : public IReader {

	ttstr dat;
	const tjs_char *p;
	ULONG length;
	ULONG pos;

public:
	IStringReader(const tjs_char *str) {
		dat = str;
		p = dat.c_str();
		length = dat.length();
		pos = 0;
	}

	void close() {
	}
	
	int getc() {
		return pos < length ? p[pos++] : EOF;
	}
	
	void ungetc() {
		if (pos > 0) {
			pos--;
		}
	}
};

// -----------------------------------------------------------------

#include "Writer.hpp"

//---------------------------------------------------------------------------

// Array クラスメンバ
static iTJSDispatch2 *ArrayCountProp   = NULL;   // Array.count

// -----------------------------------------------------------------

static void
addMethod(iTJSDispatch2 *dispatch, const tjs_char *methodName, tTJSDispatch *method)
{
	tTJSVariant var = tTJSVariant(method);
	method->Release();
	dispatch->PropSet(
		TJS_MEMBERENSURE, // メンバがなかった場合には作成するようにするフラグ
		methodName, // メンバ名 ( かならず TJS_W( ) で囲む )
		NULL, // ヒント ( 本来はメンバ名のハッシュ値だが、NULL でもよい )
		&var, // 登録する値
		dispatch // コンテキスト
		);
}

static void
delMethod(iTJSDispatch2 *dispatch, const tjs_char *methodName)
{
	dispatch->DeleteMember(
		0, // フラグ ( 0 でよい )
		methodName, // メンバ名
		NULL, // ヒント
		dispatch // コンテキスト
		);
}

static iTJSDispatch2*
getMember(iTJSDispatch2 *dispatch, const tjs_char *name)
{
	tTJSVariant val;
	if (TJS_FAILED(dispatch->PropGet(TJS_IGNOREPROP,
									 name,
									 NULL,
									 &val,
									 dispatch))) {
		ttstr msg = TJS_W("can't get member:");
		msg += name;
		TVPThrowExceptionMessage(msg.c_str());
	}
	return val.AsObject();
}

// -----------------------------------------------------------------

static tjs_error eval(IReader &file, tTJSVariant *result)
{
	tjs_error ret = TJS_S_OK;
	if (result) {
		file.parse(*result);
	}
	file.close();
	if (file.isError) {
		TVPThrowExceptionMessage(TJS_W("JSONファイル のパースに失敗しました"));
	}
	return ret;
}

//---------------------------------------------------------------------------

/**
 * JSON を文字列から読み取る
 * @param text JSON の文字列表現
 */
class tEvalJSON : public tTJSDispatch
{
protected:
public:
	tjs_error TJS_INTF_METHOD FuncCall(
		tjs_uint32 flag, const tjs_char * membername, tjs_uint32 *hint,
		tTJSVariant *result,
		tjs_int numparams, tTJSVariant **param, iTJSDispatch2 *objthis) {

		if (membername) return TJS_E_MEMBERNOTFOUND;
		if (numparams < 1) return TJS_E_BADPARAMCOUNT;

		IStringReader reader(param[0]->GetString());
		eval(reader, result);
		return TJS_S_OK;
	}
};

/**
 * JSON をファイルから読み取る
 * @param filename ファイル名
 */
class tEvalJSONStorage : public tTJSDispatch
{
protected:
public:
	tjs_error TJS_INTF_METHOD FuncCall(
		tjs_uint32 flag, const tjs_char * membername, tjs_uint32 *hint,
		tTJSVariant *result,
		tjs_int numparams, tTJSVariant **param, iTJSDispatch2 *objthis) {

		if (membername) return TJS_E_MEMBERNOTFOUND;
		if (numparams < 1) return TJS_E_BADPARAMCOUNT;

		tTJSVariantString *filename = param[0]->AsStringNoAddRef();
		bool utf8 = numparams >= 2 ? (int*)param[1] != 0 : false;

		IFileReader reader(filename, utf8);
		eval(reader, result);
		return TJS_S_OK;
	}
};

static void
quoteString(const tjs_char *str, IWriter *writer)
{
	if (str) {
		writer->write((tjs_char)'"');
		const tjs_char *p = str;
		int ch;
		while ((ch = *p++)) {
			if (ch == '"') {
				writer->write(TJS_W("\\\""));
			} else if (ch == '\\') {
			  writer->write(TJS_W("\\\\"));
			} else if (ch == 0x08) {
			  writer->write(TJS_W("\\b"));
			} else if (ch == 0x0c) {
			  writer->write(TJS_W("\\f"));
			} else if (ch == 0x0a) {
			  writer->write(TJS_W("\\n"));
			} else if (ch == 0x0d) {
			  writer->write(TJS_W("\\r"));
			} else if (ch == 0x09) {
			  writer->write(TJS_W("\\t"));
			} else if (ch < 0x20) {
			  char buf[256];
			  std::snprintf(buf, 255, "\\u%04x", ch);

			  tjs_char wbuf[256];
			  size_t l = TVPUtf8ToWideCharString(buf, wbuf);
	          wbuf[l] = (tjs_char)'\0';

			  writer->write(wbuf);
			} else {
				writer->write((tjs_char)ch);
			}
		}
		writer->write((tjs_char)'"');
	} else {
		writer->write(TJS_W("\"\""));
	}
}

static void getVariantString(tTJSVariant &var, IWriter *writer);

/**
 * 辞書の内容表示用の呼び出しロジック
 */
class DictMemberDispCaller : public tTJSDispatch /** EnumMembers 用 */
{
protected:
	IWriter *writer;
	bool first;
public:
	DictMemberDispCaller(IWriter *writer) : writer(writer) { first = true; };
	virtual tjs_error TJS_INTF_METHOD FuncCall( // function invocation
												tjs_uint32 flag,			// calling flag
												const tjs_char * membername,// member name ( NULL for a default member )
												tjs_uint32 *hint,			// hint for the member name (in/out)
												tTJSVariant *result,		// result
												tjs_int numparams,			// number of parameters
												tTJSVariant **param,		// parameters
												iTJSDispatch2 *objthis		// object as "this"
												) {
		if (numparams > 1) {
			tTVInteger flag = param[1]->AsInteger();
			if (!(flag & TJS_HIDDENMEMBER)) {
				if (first) {
					first = false;
				} else {
					writer->write((tjs_char)',');
					writer->newline();
				}
				quoteString(param[0]->GetString(), writer);
				writer->write((tjs_char)':');
				getVariantString(*param[2], writer);
			}
		}
		if (result) {
			*result = true;
		}
		return TJS_S_OK;
	}
};

static void getDictString(iTJSDispatch2 *dict, IWriter *writer)
{
	writer->write((tjs_char)'{');
	writer->addIndent();
	DictMemberDispCaller *caller = new DictMemberDispCaller(writer);
	tTJSVariantClosure closure(caller);
	dict->EnumMembers(TJS_IGNOREPROP, &closure, dict);
	caller->Release();
	writer->delIndent();
	writer->write((tjs_char)'}');
}

static void getArrayString(iTJSDispatch2 *array, IWriter *writer)
{
	writer->write((tjs_char)'[');
	writer->addIndent();
	tjs_int count = 0;
	{
		tTJSVariant result;
		if (TJS_SUCCEEDED(ArrayCountProp->PropGet(0, NULL, NULL, &result, array))) {
			count = result.AsInteger();
		}
	}
	for (tjs_int i=0; i<count; i++) {
		if (i != 0) {
			writer->write((tjs_char)',');
			writer->newline();
		}
		tTJSVariant result;
		if (array->PropGetByNum(TJS_IGNOREPROP, i, &result, array) == TJS_S_OK) {
			getVariantString(result, writer);
		}
	}
	writer->delIndent();
	writer->write((tjs_char)']');
}

static void
getVariantString(tTJSVariant &var, IWriter *writer)
{
	switch(var.Type()) {

	case tvtVoid:
		writer->write(TJS_W("null"));
		break;
		
	case tvtObject:
		{
			iTJSDispatch2 *obj = var.AsObjectNoAddRef();
			if (obj == NULL) {
				writer->write(TJS_W("null"));
			} else if (obj->IsInstanceOf(TJS_IGNOREPROP,NULL,NULL,TJS_W("Array"),obj) == TJS_S_TRUE) {
				getArrayString(obj, writer);
			} else {
				getDictString(obj, writer);
			}
		}
		break;
		
	case tvtString:
		quoteString(var.GetString(), writer);
		break;

	case tvtInteger:
		writer->write((tTVInteger)var);
		break;

	case tvtReal: {
	  ttstr str = var;
	  // delete top '+' of number.
	  if (str[0] == L'+') {
	    ttstr src = str;
	    str = src.c_str() + 1;
	  }
	  writer->write(str.c_str());
	  break;
	}

	default:
		writer->write(TJS_W("null"));
		break;
	};
}

/**
 *
 */
class tSaveJSON : public tTJSDispatch
{
protected:
public:
	tjs_error TJS_INTF_METHOD FuncCall(
		tjs_uint32 flag, const tjs_char * membername, tjs_uint32 *hint,
		tTJSVariant *result,
		tjs_int numparams, tTJSVariant **param, iTJSDispatch2 *objthis) {
		if (numparams < 2) return TJS_E_BADPARAMCOUNT;
		IFileWriter writer(param[0]->GetString(), 
			numparams > 2 ? (int)*param[2] != 0 : false, 
			numparams > 3 ? (int)*param[3] : 0);
		getVariantString(*param[1], &writer);
		return TJS_S_OK;
	}
};

/**
 *
 */
class tToJSONString : public tTJSDispatch
{
protected:
public:
	tjs_error TJS_INTF_METHOD FuncCall(
		tjs_uint32 flag, const tjs_char * membername, tjs_uint32 *hint,
		tTJSVariant *result,
		tjs_int numparams, tTJSVariant **param, iTJSDispatch2 *objthis) {
		if (numparams < 1) return TJS_E_BADPARAMCOUNT;
		if (result) {
			IStringWriter writer(numparams > 1 ? (tjs_int)*param[1] : 0);
			getVariantString(*param[0], &writer);
			*result = writer.buf;
		}
		return TJS_S_OK;
	}
};


void json_init()
{
	// Arary クラスメンバー取得
	{
		tTJSVariant varScripts;
		TVPExecuteExpression(TJS_W("Array"), &varScripts);
		iTJSDispatch2 *dispatch = varScripts.AsObjectNoAddRef();
		// メンバ取得
		ArrayCountProp = getMember(dispatch, TJS_W("count"));
	}

	{
		tTJSVariant varScripts;
		TVPExecuteExpression(TJS_W("Scripts"), &varScripts);
		iTJSDispatch2 *dispatch = varScripts.AsObjectNoAddRef();
		if (dispatch) {
			addMethod(dispatch, TJS_W("evalJSON"),        new tEvalJSON());
			addMethod(dispatch, TJS_W("evalJSONStorage"), new tEvalJSONStorage());
			addMethod(dispatch, TJS_W("saveJSON"),        new tSaveJSON());
			addMethod(dispatch, TJS_W("toJSONString"),    new tToJSONString());
		}
	}
}

//---------------------------------------------------------------------------

#ifdef TVP_STATIC_PLUGIN

#define EXPORT(hr) static hr STDCALL

#else

#if defined(_MSC_VER)
    #define DLL_EXPORT  __declspec(dllexport)
#else
	#define DLL_EXPORT  __attribute__((visibility("default")))
#endif

#define EXPORT(hr) extern "C" DLL_EXPORT hr STDCALL

#ifdef _MSC_VER
# if defined(_M_AMD64) || defined(_M_X64)
#  pragma comment(linker, "/EXPORT:V2Link")
#  pragma comment(linker, "/EXPORT:V2Unlink")
# else
#  pragma comment(linker, "/EXPORT:V2Link=_V2Link@4")
#  pragma comment(linker, "/EXPORT:V2Unlink=_V2Unlink@0")
# endif
#endif
#ifdef __GNUC__
asm (".section .drectve");
# if defined(__x86_64__) || defined(__x86_64)
asm (".ascii \" -export:V2Link=V2Link -export:V2Unlink=V2Unlink\"");
# else
asm (".ascii \" -export:V2Link=V2Link@4 -export:V2Unlink=V2Unlink@0\"");
# endif
#endif

extern "C"
int WINAPI DllEntryPoint(HINSTANCE hinst, unsigned long reason, void* lpReserved)
{
	return 1;
}

#endif

//---------------------------------------------------------------------------
static tjs_int GlobalRefCountAtInit = 0;

EXPORT(HRESULT) V2Link(iTVPFunctionExporter *exporter)
{
	// スタブの初期化(必ず記述する)
	TVPInitImportStub(exporter);

	json_init();

	// この時点での TVPPluginGlobalRefCount の値を
	GlobalRefCountAtInit = TVPPluginGlobalRefCount;
	// として控えておく。TVPPluginGlobalRefCount はこのプラグイン内で
	// 管理されている tTJSDispatch 派生オブジェクトの参照カウンタの総計で、
	// 解放時にはこれと同じか、これよりも少なくなってないとならない。
	// そうなってなければ、どこか別のところで関数などが参照されていて、
	// プラグインは解放できないと言うことになる。

	return S_OK;
}
//---------------------------------------------------------------------------
static void TJS_USERENTRY tryUnlinkScripts(void *data)
{
  tTJSVariant varScripts;
  TVPExecuteExpression(TJS_W("Scripts"), &varScripts);
  iTJSDispatch2 *dispatch = varScripts.AsObjectNoAddRef();
  if (dispatch) {
    delMethod(dispatch, TJS_W("evalJSON"));
    delMethod(dispatch, TJS_W("evalJSONStorage"));
  }
}

static bool TJS_USERENTRY catchUnlinkScripts(void *data, const tTVPExceptionDesc & desc) {
  return false;
}

EXPORT(HRESULT) V2Unlink()
{
	// 吉里吉里側から、プラグインを解放しようとするときに呼ばれる関数。

	// もし何らかの条件でプラグインを解放できない場合は
	// この時点で E_FAIL を返すようにする。
	// ここでは、TVPPluginGlobalRefCount が GlobalRefCountAtInit よりも
	// 大きくなっていれば失敗ということにする。
	if(TVPPluginGlobalRefCount > GlobalRefCountAtInit) return E_FAIL;
	// E_FAIL が帰ると、Plugins.unlink メソッドは偽を返す

	{
          TVPDoTryBlock(&tryUnlinkScripts, &catchUnlinkScripts, NULL, NULL);
	}
	
	if (ArrayCountProp) {
		ArrayCountProp->Release();
		ArrayCountProp = NULL;
	}
	
	// スタブの使用終了(必ず記述する)
	TVPUninitImportStub();

	return S_OK;
}

#ifdef TVP_STATIC_PLUGIN

#if defined(_MSC_VER)
    #define EXPORT_USED __declspec(dllexport)
#else
	#define EXPORT_USED __attribute__((visibility("default"), used))
#endif

#define str(x) TJS_W(#x)
#define strx(x) str(x)
#define CAT(a, b) a##b
#define XCAT(a, b) CAT(a, b)
#define MAKE_FUNC(name) XCAT(krkrz_plugin_, name)

// リンク用エントリ関数
// _krkrz_plugin_プロジェクト名 で関数が作られる
extern "C" EXPORT_USED void STDCALL MAKE_FUNC(TVP_PLUGIN_NAME)() {
	static iTVPStaticPlugin plugin;
    plugin.name = strx(TVP_PLUGIN_NAME);
	plugin.link = (int32_t (STDCALL *)(iTVPFunctionExporter *))V2Link;
	plugin.unlink = (int32_t (STDCALL *)(void))V2Unlink;
	TVPRegisterPlugin(&plugin);
}

#endif
