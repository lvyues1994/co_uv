#include <iostream>
#include <string>

int main() {
    std::string all;
    std::string line;
    while (std::getline(std::cin, line)) {
        all += line;
        all += '\n';
    }
    std::cout << "echo:" << all;
    return 0;
}
