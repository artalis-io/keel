// F2-A negative canary: this TU does NOT include a Keel header. It hand-declares a Keel function
// WITHOUT an extern "C" guard, so in C++ the name gets C++ language linkage and is mangled
// (e.g. _Z18kl_version_numberv). The C-built libkeel.a exports the unmangled C symbol
// kl_version_number, so this reference is UNRESOLVED and the link MUST FAIL. That failure proves the
// header's extern "C" guard is exactly what makes C++ linkage against the C archive succeed;
// tools/f2_cxx_link_test.sh asserts this link fails.
int kl_version_number(void); // no extern "C" on purpose

int main(void) {
    return kl_version_number() > 0 ? 0 : 1;
}
