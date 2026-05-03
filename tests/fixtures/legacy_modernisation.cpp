#define NULL 0
namespace std { template <class T> class auto_ptr {}; }

int* legacy_pointer() throw() {
    register int value = 0;
    int* pointer = NULL;
    typedef int LegacyInt;
    LegacyInt casted = (int)3.0;
    int* raw = new int;
    delete raw;
    return pointer;
}

void ownership() {
    std::auto_ptr<int> owned;
}
