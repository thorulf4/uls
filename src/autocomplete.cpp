#include <uls/autocomplete.h>
#include <uls/server.h>
#include <uls/common_data.h>
#include <uls/declarations.h>
#include <uls/utap_extension.h>
#include <sstream>
#include <set>
#include <cctype>
#include <array>
#include <iostream>
#include <algorithm>
#include <ranges>
#include <functional>
#include <iterator>

namespace ranges = std::ranges;

// Assigned to unique bits to allow filters to be represented as bitmasks
enum SymType : uint8_t { function = 1, variable = 2, channel = 4, type = 8, process = 16, unknown = 32 };

SymType sym_type(const UTAP::type_t& type)
{
    if (type.is_channel())
        return SymType::channel;
    if (type.is_clock() || type.is_integral() || type.is_double() || type.is_string() || type.is_record())
        return SymType::variable;
    if (type.is(UTAP::Constants::TYPEDEF))
        return SymType::type;
    if (type.is_function() || type.is_function_external())
        return SymType::function;
    if (type.is_array())
        return sym_type(type.get(0));
    return SymType::unknown;
}

struct Suggestion
{
    std::string name;
    SymType type;
};

const auto guard_items = std::array{
    Suggestion{"true", SymType::unknown},
    Suggestion{"false", SymType::unknown},
};

const auto queries_items = std::array{
    Suggestion{"int", SymType::type},       Suggestion{"true", SymType::unknown},
    Suggestion{"false", SymType::unknown},  Suggestion{"forall", SymType::unknown},
    Suggestion{"exists", SymType::unknown},
};

const auto parameter_items =
    std::array{Suggestion{"int", SymType::type},      Suggestion{"double", SymType::type},
               Suggestion{"clock", SymType::type},    Suggestion{"chan", SymType::type},
               Suggestion{"bool", SymType::type},     Suggestion{"broadcast", SymType::unknown},
               Suggestion{"const", SymType::unknown}, Suggestion{"urgent", SymType::unknown}};

const auto default_items = std::array{Suggestion{"int", SymType::type},       Suggestion{"double", SymType::type},
                                      Suggestion{"clock", SymType::type},     Suggestion{"chan", SymType::type},
                                      Suggestion{"bool", SymType::type},      Suggestion{"broadcast", SymType::unknown},
                                      Suggestion{"const", SymType::unknown},  Suggestion{"urgent", SymType::unknown},
                                      Suggestion{"void", SymType::unknown},   Suggestion{"meta", SymType::unknown},
                                      Suggestion{"true", SymType::unknown},   Suggestion{"false", SymType::unknown},
                                      Suggestion{"forall", SymType::unknown}, Suggestion{"exists", SymType::unknown},
                                      Suggestion{"return", SymType::unknown}, Suggestion{"typedef", SymType::unknown},
                                      Suggestion{"struct", SymType::unknown}};

const auto builtin_functions = std::array{Suggestion{"abs", SymType::function},
                                          Suggestion{"fabs", SymType::function}, Suggestion{"fmod", SymType::function},
                                          Suggestion{"fma", SymType::function}, Suggestion{"fmax", SymType::function},
                                          Suggestion{"fmin", SymType::function}, Suggestion{"exp", SymType::function},
                                          Suggestion{"exp2", SymType::function}, Suggestion{"expm1", SymType::function},
                                          Suggestion{"ln", SymType::function}, Suggestion{"log", SymType::function},
                                          Suggestion{"log10", SymType::function}, Suggestion{"log2", SymType::function},
                                          Suggestion{"log1p", SymType::function}, Suggestion{"pow", SymType::function},
                                          Suggestion{"sqrt", SymType::function}, Suggestion{"cbrt", SymType::function},
                                          Suggestion{"hypot", SymType::function}, Suggestion{"sin", SymType::function},
                                          Suggestion{"cos", SymType::function}, Suggestion{"tan", SymType::function},
                                          Suggestion{"asin", SymType::function}, Suggestion{"acos", SymType::function},
                                          Suggestion{"atan", SymType::function}, Suggestion{"atan2", SymType::function},
                                          Suggestion{"sinh", SymType::function}, Suggestion{"cosh", SymType::function},
                                          Suggestion{"tanh", SymType::function}, Suggestion{"asinh", SymType::function},
                                          Suggestion{"acosh", SymType::function}, Suggestion{"atanh", SymType::function},
                                          Suggestion{"erf", SymType::function}, Suggestion{"erfc", SymType::function},
                                          Suggestion{"tgamma", SymType::function}, Suggestion{"lgamma", SymType::function},
                                          Suggestion{"ceil", SymType::function}, Suggestion{"floor", SymType::function},
                                          Suggestion{"trunc", SymType::function}, Suggestion{"round", SymType::function},
                                          Suggestion{"fint", SymType::function}, Suggestion{"ldexp", SymType::function},
                                          Suggestion{"ilogb", SymType::function}, Suggestion{"logb", SymType::function},
                                          Suggestion{"nextafter", SymType::function}, Suggestion{"copysign", SymType::function},
                                          Suggestion{"signbit", SymType::function}, Suggestion{"random", SymType::function},
                                          Suggestion{"random_normal", SymType::function}, Suggestion{"random_poisson", SymType::function},
                                          Suggestion{"random_arcsine", SymType::function}, Suggestion{"random_beta", SymType::function},
                                          Suggestion{"random_gamma", SymType::function}, Suggestion{"tri", SymType::function},
                                          Suggestion{"random_weibull", SymType::function}};

template <>
struct Serializer<SymType>
{
    static nlohmann::json serialize(const SymType& type)
    {
        switch (type) {
        case SymType::function: return "function";
        case SymType::variable: return "variable";
        case SymType::channel: return "channel";
        case SymType::type: return "type";
        case SymType::process: return "process";
        default: return "unknown";
        }
    }
};

template <>
struct Serializer<std::vector<Suggestion>>
{
    static nlohmann::json serialize(const std::vector<Suggestion>& items)
    {
        nlohmann::json json_array;
        for (const Suggestion& item : items) {
            json_array.push_back(
                nlohmann::json{{"name", item.name}, {"type", Serializer<SymType>::serialize(item.type)}});
        }
        return json_array;
    }
};

bool is_struct(const UTAP::symbol_t& sym)
{
    return sym.get_type().size() == 1 && sym.get_type().get(0).is(UTAP::Constants::RECORD);
}

bool is_template(const UTAP::symbol_t& sym) { return sym.get_type().is(UTAP::Constants::INSTANCE); }

bool is_digit(unsigned char c) { return std::isdigit(c); }

// Uppaal will name unnamed locations _id0, _id1, _id2, etc. check for this pattern
bool is_name_autogenerated(std::string_view name)
{
    return name.substr(0, 3) == "_id" && ranges::all_of(name.substr(3), is_digit);
}

class ResultBuilder
{
    std::vector<Suggestion> items;
    std::string prefix;
    uint8_t type_filter_mask;

    // Takes stl containers of Suggestions
    void add_items(const auto& ... container) { 
        auto type_check = [filter=type_filter_mask](const Suggestion& s) { return (filter & s.type) == 0U; };
        (ranges::copy_if(container, std::back_inserter(items), type_check), ...); 
    }

public:
    void set_ignored_mask(uint8_t ignore_mask) { type_filter_mask = ignore_mask; }
    void add_defaults(std::string_view xpath)
    {
        if (xpath.ends_with("/queries!"))
            add_items(queries_items, builtin_functions);
        else if (xpath.ends_with("/parameter!"))
            add_items(parameter_items);
        else if (xpath.ends_with("label[@kind=\"guard\"]"))
            add_items(guard_items, builtin_functions);
        else
            add_items(default_items, builtin_functions);
    }
    void set_prefix(std::string new_prefix) { prefix = std::move(new_prefix); }
    void add_struct(const UTAP::type_t& type)
    {
        if (type.size() == 0)
            return;
        if (type.get_kind() != UTAP::Constants::RECORD) {
            add_struct(type.get(0));
            return;
        }

        for (size_t i = 0; i < type.size(); i++) {
            add_item(prefix + type.get_label(i), SymType::variable);
        }
    }

    void add_template(const UTAP::template_t& templ)
    {
        for (const UTAP::variable_t& var : templ.variables) {
            add_item(prefix + var.uid.get_name(), SymType::variable);
        }
        for (const UTAP::function_t& func : templ.functions) {
            add_item(prefix + func.uid.get_name(), SymType::function);
        }
        for (const UTAP::location_t& loc : templ.locations) {
            std::string_view name = loc.uid.get_name();
            if (!is_name_autogenerated(name))
                add_item(std::string{prefix}.append(name), SymType::unknown);
        }
    }

    void add_item(std::string item, SymType type)
    {
        if ((type_filter_mask & type) == 0U)
            items.emplace_back(std::move(item), type);
    }

    std::vector<Suggestion>& get_items() { return items; }
};

void AutocompleteModule::configure(Server& server)
{
    server.add_command<Identifier>("autocomplete", [this](const Identifier& id) -> std::vector<Suggestion> {
        auto results = ResultBuilder{};

        if (id.xpath.ends_with("/parameter!"))
            results.set_ignored_mask(~SymType::type);
        else if (id.xpath.ends_with("label[@kind=\"invariant\"]"))
            results.set_ignored_mask(~(SymType::variable | SymType::function));
        else if (id.xpath.ends_with("label[@kind=\"exponentialrate\"]"))
            results.set_ignored_mask(~SymType::variable);
        else if (id.xpath.ends_with("label[@kind=\"select\"]"))
            results.set_ignored_mask(~SymType::type);
        else if (id.xpath.ends_with("label[@kind=\"guard\"]"))
            results.set_ignored_mask(~(SymType::variable | SymType::function));
        else if (id.xpath.ends_with("label[@kind=\"synchronisation\"]"))
            results.set_ignored_mask(~SymType::channel);
        else if (id.xpath.ends_with("label[@kind=\"assignment\"]"))
            results.set_ignored_mask(~(SymType::variable | SymType::function));

        bool is_query = id.xpath == "/nta/queries!";
        UTAP::Document& doc = doc_repo.get_document();
        UTAP::declarations_t& decls = navigate_xpath(doc, id.xpath, id.offset);

        auto offset = id.identifier.find_last_of('.');
        if (offset != std::string::npos) {
            if (std::optional<UtapEntity> entity = find_declaration(doc, decls, id.identifier.substr(0, offset))) {
                results.set_prefix(id.identifier.substr(0, offset + 1));
                std::visit(overloaded{[&](UTAP::symbol_t& sym) {
                                        if (is_template(sym) && is_query){
                                            if(auto process = find_process(doc, sym.get_name()))
                                                results.add_template(*process);
                                        } else if (is_struct(sym))
                                            results.add_struct(sym.get_type().get(0));
                                      },
                                      [&](UTAP::type_t& type) { results.add_struct(type); }},
                           *entity);
            }
        } else {
            results.add_defaults(id.xpath);
            bool use_templates = is_query || id.xpath == "/nta/system!";
            DeclarationsWalker{doc, false}.visit_symbols(decls, [&](UTAP::symbol_t& symbol, const TextRange& range) {
                bool is_symbol_visible = symbol.get_frame() != decls.frame || range.begOffset < id.offset;
                if (is_symbol_visible) {
                    if (!is_template(symbol))
                        results.add_item(symbol.get_name(), sym_type(symbol.get_type()));
                    else if (use_templates)
                        results.add_item(symbol.get_name(), SymType::process);
                }
                return false;
            });
        }

        return std::move(results.get_items());
    });
}