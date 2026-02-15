#include "cnf.hpp"
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
namespace {
std::string trim_left(const std::string& value) {
  std::size_t i = 0;
  while (i < value.size() && std::isspace(static_cast<unsigned char>(value[i])) != 0) {
    ++i;
  }
  return value.substr(i);
}
}
CnfFormula parse_dimacs_cnf(const std::string& path) {
  std::ifstream input(path);
  if (!input.is_open()) {
    throw std::runtime_error("Nu pot deschide fisierul CNF: " + path);
  }
  CnfFormula formula;
  bool saw_header = false;
  int expected_clauses = -1;
  Clause current_clause;
  std::string line;
  while (std::getline(input, line)) {
    const std::string t = trim_left(line);
    if (t.empty() || t[0] == 'c') {
      continue;
    }
    if (t[0] == 'p') {
      std::istringstream header(t);
      std::string p_token;
      std::string format;
      int vars = 0;
      int clauses = 0;
      header >> p_token >> format >> vars >> clauses;
      if (p_token != "p" || format != "cnf" || vars <= 0 || clauses < 0) {
        throw std::runtime_error("Header DIMACS invalid in: " + path);
      }
      formula.num_variables = vars;
      expected_clauses = clauses;
      saw_header = true;
      continue;
    }
    if (!saw_header) {
      throw std::runtime_error("Fisier DIMACS fara header 'p cnf': " + path);
    }
    std::istringstream literals_stream(t);
    int literal = 0;
    while (literals_stream >> literal) {
      if (literal == 0) {
        if (current_clause.literals.empty()) {
          throw std::runtime_error("Clauza vida detectata in: " + path);
        }
        formula.clauses.push_back(current_clause);
        current_clause = Clause{};
        continue;
      }
      if (std::abs(literal) > formula.num_variables) {
        throw std::runtime_error("Literal in afara intervalului declarat in: " + path);
      }
      current_clause.literals.push_back(literal);
    }
  }
  if (!current_clause.literals.empty()) {
    throw std::runtime_error("Ultima clauza nu se termina cu 0 in: " + path);
  }
  if (!saw_header) {
    throw std::runtime_error("Header DIMACS lipsa in: " + path);
  }
  if (expected_clauses >= 0 && static_cast<int>(formula.clauses.size()) != expected_clauses) {
    throw std::runtime_error("Numar clauze mismatch: header=" + std::to_string(expected_clauses) +
                             ", parsed=" + std::to_string(formula.clauses.size()));
  }
  return formula;
}
