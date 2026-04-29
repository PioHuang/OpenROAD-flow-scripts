#ifndef PDN_BUILDER_HPP
#define PDN_BUILDER_HPP

#include <pdn/network.hpp>
#include <pdn/spec.hpp>

namespace pdn {

class PdnBuilder {
 public:
  static ResistiveNetwork build(const PdnProblem& problem);
};

}  // namespace pdn

#endif  // PDN_BUILDER_HPP
