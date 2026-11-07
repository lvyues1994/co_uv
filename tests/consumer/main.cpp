#include <uvco/uvco.hpp>
co2::Task<int> other(uvco::Loop&);
int main() { uvco::Loop loop; return uvco::run(loop,other(loop))==7 ? 0 : 1; }
