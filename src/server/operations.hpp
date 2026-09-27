// XRM: Declares asynchronous operation records and lookup.

#pragma once

#include "model/common.hpp"

#include <deque>
#include <map>

namespace xrm {

class Operations {
public:
  explicit Operations(Id boot_id);
  Id begin();
  void complete(const Id &, Result<Json> result);
  Result<Json> get(const Id &) const;

private:
  struct Record {
    std::string status = "running";
    std::optional<Json> result;
    std::optional<AppError> error;
  };
  void prune();
  Id boot_id_;
  std::uint64_t sequence_ = 1;
  std::map<Id, Record, std::less<>> records_;
  std::deque<Id> terminal_;
};

} // namespace xrm
