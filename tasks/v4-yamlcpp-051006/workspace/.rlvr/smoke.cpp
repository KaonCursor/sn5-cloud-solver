#include <yaml-cpp/yaml.h>
#include <iostream>

int main() {
  YAML::Emitter out;
  out << YAML::Load("{a: [1, 2], b: {c: x}}");
  std::cout << out.c_str() << std::endl;
  return 0;
}
