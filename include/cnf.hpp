#pragma once
#include <string>
#include <vector>
struct Clause {
  std::vector<int> literals;
};
struct CnfFormula {
  int num_variables = 0;
  std::vector<Clause> clauses;
};
CnfFormula parse_dimacs_cnf(const std::string& path);
