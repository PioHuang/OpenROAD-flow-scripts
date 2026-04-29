#ifndef PDN_IO_HPP
#define PDN_IO_HPP

#include <pdn/spec.hpp>

#include <filesystem>

namespace pdn {

PdnProblem loadProblemFromJsonFile(const std::filesystem::path& path);

}  // namespace pdn

#endif  // PDN_IO_HPP
