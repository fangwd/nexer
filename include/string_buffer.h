/* Copyright (c) Weidong Fang. All rights reserved. */

#ifndef STRING_BUFFER_H_
#define STRING_BUFFER_H_

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ostream>
#include <streambuf>
#include <string>

class StringBuffer : public std::ostream {
    class MyBuf : public std::streambuf {
      private:
        char *base;
        size_t cap;

      public:
        MyBuf() : base(0), cap(0) {
            setp(base, base + cap);
        }

        ~MyBuf() {
            if (base) free(base);
        }

        size_t size() {
            return pptr() - pbase();
        }

        void Resize(size_t new_size) {
            size_t old_size = size();

            if (new_size < old_size) {
                memset(base + new_size, 0, old_size - new_size);
            } else if (new_size > cap) {
                Reserve(new_size);
            }
            setp(base, base + cap);
            pbump(new_size);
        }

        void Consume(size_t n) {
            if (n > 0) {
                memmove(base, base + n, size() - n);
                Resize(size() - n);
            }
        }

        void Clear() {
            memset(base, 0, cap);
            setp(base, base + cap);
        }

        virtual int_type overflow(int_type c) {
            if (c != EOF) {
                Reserve(cap + 1);
                *pptr() = c;
                pbump(1);
            }
            return c;
        }

        virtual std::streamsize xsputn(const char *s, std::streamsize n) {
            return (std::streamsize)Write(s, (size_t)n);
        }

        int Reserve(size_t new_cap) {
            static const size_t MIN_SIZE = 512;
            static const size_t MAX_SIZE = 268435456;

            if (cap < new_cap) {
                new_cap += MIN_SIZE - 1;
                new_cap /= MIN_SIZE;
                new_cap *= MIN_SIZE;
                if (new_cap <= MAX_SIZE) {
                    char *new_base = (char *)realloc(base, new_cap);
                    if (new_base != NULL) {
                        int size = pptr() - pbase();
                        base = new_base;
                        cap = new_cap;
                        memset(base + size, '\0', cap - size);
                        setp(base, base + cap);
                        pbump(size);
                    } else {
                        return -1;
                    }
                } else {
                    return -2;
                }
            }

            return 0;
        }

        size_t Write(const char *s, size_t len) {
            size_t size = this->size();
            if (size + len + 1 > cap) {
                if (Reserve(size + len + 1) != 0) {
                    return (size_t)-1;
                }
            }
            memcpy(base + size, s, len);
            pbump((int)len);
            return len;
        }

        friend class StringBuffer;
    };

    MyBuf *mb;

    StringBuffer(const StringBuffer &);
    StringBuffer &operator=(const StringBuffer &);

    int vprintf(const char *fmt, va_list ap) {
        size_t size = mb->size();

        while (1) {
            va_list cp;
            int m = mb->cap - size;
            va_copy(cp, ap);
            int n = vsnprintf(mb->base + size, m, fmt, cp);
            va_end(cp);
            if (n > -1 && n < m) {
                mb->pbump(n);
                return n;
            }
            m = n > -1 ? n + 1 : 2 * m;
            if (size + m + 1 > mb->cap) {
                if (mb->Reserve(size + m + 1) != 0) {
                    return -1;
                }
            }
        }

        return -1;
    }

    bool read(FILE *fp, size_t n) {
        mb->Reserve(mb->size() + n);
        if (fread(mb->base + mb->size(), 1, n, fp) == n) {
            mb->Resize(mb->size() + n);
            return true;
        }
        return false;
    }

  public:
    StringBuffer() : std::ostream(new MyBuf()) {
        mb = (MyBuf *)rdbuf();
    }

    ~StringBuffer() {
        delete mb;
    }

    const char *data() const {
        return mb->base;
    }

    char *data() {
        return mb->base;
    }

    size_t size() const {
        return mb->size();
    }

    void Resize(size_t new_size) {
        mb->Resize(new_size);
    }

    void Consume(size_t n) {
        return mb->Consume(n);
    }

    StringBuffer &Clear() {
        mb->Clear();
        return *this;
    }

    StringBuffer &Write(const char *s, size_t n = 0) {
        if (n == 0) {
            n = strlen(s);
        }
        mb->Write(s, n);
        return *this;
    }

    StringBuffer &Write(int ch) {
        char c = (char)ch;
        return Write(&c, 1);
    }

    int Printf(const char *fmt, ...) {
        va_list args;
        va_start(args, fmt);
        int n = vprintf(fmt, args);
        va_end(args);
        return n;
    }

    bool Load(const char *filename) {
        FILE *fp;
        if ((fp = fopen(filename, "rb")) != NULL) {
            fseek(fp, 0, SEEK_END);
            size_t size = (size_t)ftell(fp);
            fseek(fp, 0, SEEK_SET);
            mb->Clear();
            bool result = read(fp, size);
            fclose(fp);
            return result;
        }
        return false;
    }

    bool Save(const char *filename) {
        FILE *fp;
        if ((fp = fopen(filename, "wb")) != NULL) {
            if (fwrite(mb->base, 1, mb->size(), fp) != mb->size()) {
                fclose(fp);
                return false;
            }
            fclose(fp);
            return true;
        }
        return false;
    }

    bool operator==(const char *s) const {
        if (s && mb->size() == strlen(s))
            return !memcmp(mb->base, s, mb->size());
        else
            return false;
    }

    char &operator[](int n) {
        if (n < 0) {
            n += size();
        }
        return mb->base[n];
    }

    bool Reserve(size_t n) {
        size_t space_size = mb->cap - mb->size();
        if (space_size < n) {
            return mb->Reserve(mb->size() + n) ? false : true;
        }
        return true;
    }

    void appended(size_t n) {
        mb->pbump((int)n);
    }

    bool ends_with(const char *s, size_t len = 0) {
        if (len == 0) len = strlen(s);
        if (mb->size() < len) return false;
        return !memcmp(mb->base + mb->size() - len, s, len);
    }
};

inline std::ostream &operator<<(std::ostream &os, const StringBuffer &mb) {
    os.write(mb.data(), mb.size());
    return os;
}

#endif /* STRING_BUFFER_H_ */
