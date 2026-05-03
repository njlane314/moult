#define NULL 0
namespace std { template <class T> class auto_ptr {}; }

int* legacy_pointer() throw() {
    register int value = 0;
    int* pointer = NULL;
    return pointer;
}

void ownership() {
    std::auto_ptr<int> owned;
}
