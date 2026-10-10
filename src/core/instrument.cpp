#include "pulse/core/instrument.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace pulse {

std::string_view InstrumentSpec::symbol_view() const noexcept {
  const auto* end = static_cast<const char*>(std::memchr(symbol.data(), '\0', symbol.size()));
  return {symbol.data(), end ? static_cast<std::size_t>(end - symbol.data()) : symbol.size()};
}

InstrumentSpec make_instrument(InstrumentId id, std::string_view symbol, Price reference_price,
                               Price tick_size, double band_fraction) {
  if (tick_size <= 0) throw std::invalid_argument("tick_size must be positive");
  if (reference_price % tick_size != 0) throw std::invalid_argument("reference price off tick");
  InstrumentSpec s;
  s.id = id;
  const auto n = std::min(symbol.size(), s.symbol.size());
  std::memcpy(s.symbol.data(), symbol.data(), n);
  s.tick_size = tick_size;
  s.reference_price = reference_price;
  const auto band_ticks = static_cast<Price>(
      std::floor(static_cast<double>(reference_price) * band_fraction / static_cast<double>(tick_size)));
  s.min_price = std::max(tick_size, reference_price - band_ticks * tick_size);
  s.max_price = reference_price + band_ticks * tick_size;
  return s;
}

InstrumentUniverse::InstrumentUniverse(std::vector<InstrumentSpec> specs) : specs_(std::move(specs)) {
  for (std::size_t i = 0; i < specs_.size(); ++i) {
    if (specs_[i].id != i) throw std::invalid_argument("instrument ids must be dense and ordered");
  }
}

InstrumentUniverse InstrumentUniverse::synthetic(std::size_t n) {
  static constexpr std::string_view kSymbols[] = {"ALFA", "BRVO", "CHRL", "DLTA", "ECHO", "FXTR",
                                                  "GOLF", "HOTL", "INDA", "JULT", "KILO", "LIMA",
                                                  "MIKE", "NOVM", "OSCR", "PAPA"};
  // Reference prices spread over typical equity ranges, all on a 0.01 tick.
  static constexpr Price kRefs[] = {1'000'000, 450'000, 1'945'000, 88'000, 250'000, 3'120'000,
                                    72'000,    515'000, 1'230'000, 39'000, 610'000, 2'200'000,
                                    150'000,   980'000, 330'000,   47'500};
  std::vector<InstrumentSpec> specs;
  specs.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    char buf[24];
    std::string_view sym;
    if (i < std::size(kSymbols)) {
      sym = kSymbols[i];
    } else {
      std::snprintf(buf, sizeof(buf), "S%05zu", i);
      sym = buf;
    }
    specs.push_back(make_instrument(static_cast<InstrumentId>(i), sym, kRefs[i % std::size(kRefs)]));
  }
  return InstrumentUniverse(std::move(specs));
}

std::optional<InstrumentId> InstrumentUniverse::find(std::string_view symbol) const noexcept {
  for (const auto& s : specs_) {
    if (s.symbol_view() == symbol) return s.id;
  }
  return std::nullopt;
}

std::string format_price(Price p) {
  if (p == kInvalidPrice) return "-";
  char buf[32];
  const bool neg = p < 0;
  const auto a = static_cast<std::uint64_t>(neg ? -p : p);
  std::snprintf(buf, sizeof(buf), "%s%llu.%04llu", neg ? "-" : "",
                static_cast<unsigned long long>(a / kPriceScale),
                static_cast<unsigned long long>(a % kPriceScale));
  return buf;
}

}  // namespace pulse
