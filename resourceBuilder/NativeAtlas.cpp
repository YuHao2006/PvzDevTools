#include "NativeBuild.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <numeric>
#include <ortools/sat/cp_model.h>
#include <ortools/sat/cp_model_solver.h>
#include <ortools/sat/sat_parameters.pb.h>

namespace pvz::build
{
namespace
{
constexpr auto schema = "pvz-exact-atlas-v2";
constexpr auto modelVersion = "integer-nonrotating-padded-global-area-v2";
constexpr auto solverVersion = "9.15.6755";
using Clock = std::chrono::steady_clock;

std::string Canonical(const Json &value, bool spaces = false)
{
    if (value.is_object())
    {
        std::map<std::string, Json> sorted;
        for (auto &[key, v] : value.items())
            sorted[key] = v;
        std::string out = "{";
        bool first = true;
        for (auto &[key, v] : sorted)
        {
            if (!first)
                out += spaces ? ", " : ",";
            first = false;
            out += Json(key).dump(-1, ' ', true) + (spaces ? ": " : ":") + Canonical(v, spaces);
        }
        return out + "}";
    }
    if (value.is_array())
    {
        std::string out = "[";
        bool first = true;
        for (auto &v : value)
        {
            if (!first)
                out += spaces ? ", " : ",";
            first = false;
            out += Canonical(v, spaces);
        }
        return out + "]";
    }
    return value.dump(-1, ' ', true);
}
std::string Hash(const std::string &value)
{
    return Sha256(std::span(reinterpret_cast<const unsigned char *>(value.data()), value.size()));
}
unsigned Integer(const Json &value)
{
    Require(value.is_number_integer() && !value.is_boolean() && value.get<long long>() >= 0 &&
                value.get<unsigned long long>() <= 0xffffffffULL,
            "Invalid atlas integer");
    return value.get<unsigned>();
}
void Validate(const Json &sizes, const Json &layout, unsigned padding, unsigned maxSide)
{
    auto width = Integer(layout.at("width")), height = Integer(layout.at("height"));
    const auto &regions = layout.at("regions");
    Require(regions.is_array() && regions.size() == sizes.size(), "Atlas region count differs");
    if (sizes.empty())
    {
        Require(width == 0 && height == 0, "Empty atlas has nonzero extent");
        return;
    }
    Require(width > 0 && height > 0 && width <= maxSide && height <= maxSide, "Atlas extent outside limit");
    std::vector<std::array<unsigned, 4>> occupied;
    for (std::size_t i = 0; i < sizes.size(); ++i)
    {
        const auto &r = regions[i];
        Require(r.is_array() && r.size() == 4, "Invalid atlas region");
        unsigned x = Integer(r[0]), y = Integer(r[1]), w = Integer(r[2]), h = Integer(r[3]);
        Require(w == Integer(sizes[i][0]) && h == Integer(sizes[i][1]) && x >= padding && y >= padding &&
                    x + w + padding <= width && y + h + padding <= height,
                "Atlas must preserve source orientation and padded extent");
        std::array<unsigned, 4> rectangle = {x - padding, y - padding, x + w + padding, y + h + padding};
        for (auto &old : occupied)
            Require(rectangle[0] >= old[2] || old[0] >= rectangle[2] || rectangle[1] >= old[3] ||
                        old[1] >= rectangle[3],
                    "Atlas padded regions overlap");
        occupied.push_back(rectangle);
    }
}
auto Quality(const Json &layout)
{
    unsigned w = layout.at("width"), h = layout.at("height");
    return std::array<unsigned long long, 4>{1ULL * w * h, std::max(w, h), w, h};
}
unsigned LowerBound(const Json &sizes, unsigned padding, unsigned side, unsigned upper)
{
    if (sizes.empty())
        return 0;
    unsigned minW = 0, minH = 0, total = 0;
    for (auto &pair : sizes)
    {
        unsigned w = pair[0].get<unsigned>() + 2 * padding, h = pair[1].get<unsigned>() + 2 * padding;
        minW = std::max(minW, w);
        minH = std::max(minH, h);
        total += w * h;
    }
    unsigned lower = upper;
    for (unsigned w = minW; w <= std::min(side, upper / minH); ++w)
        lower = std::min(lower, w * std::max(minH, (total + w - 1) / w));
    return lower;
}
Json Metadata(const std::string &status, unsigned bound, const Json &layout, bool optimal, double elapsed,
              const std::string &key, bool hit)
{
    unsigned area = layout["width"].get<unsigned>() * layout["height"].get<unsigned>();
    return {{"status", status},
            {"best_bound", bound},
            {"optimal", optimal},
            {"gap", area ? double(area - bound) / area : 0.0},
            {"elapsed_seconds", elapsed},
            {"cache_hit", hit},
            {"cache_key", key},
            {"solver", "ortools-cp-sat"},
            {"solver_version", solverVersion},
            {"model_version", modelVersion},
            {"search_complete", false},
            {"solve_budget_seconds", nullptr},
            {"proof_scope", "minimum integer atlas area; fixed orientation; configured padding and max_side"},
            {"objective_area", area}};
}
void Save(const fs::path &path, const std::string &key, const Json &layout)
{
    Json record = {
        {"format", schema}, {"key", key}, {"layout", layout}, {"layout_sha256", Hash(Canonical(layout))}};
    Publish(path, record.dump(2) + "\n");
}
Json Cache(const fs::path &path, const std::string &key, const Json &sizes, unsigned padding, unsigned side)
{
    try
    {
        auto record = ReadJson(path);
        Require(record.at("format") == schema && record.at("key") == key, "Cache identity mismatch");
        auto layout = record.at("layout");
        Require(record.at("layout_sha256") == Hash(Canonical(layout)), "Cache checksum mismatch");
        Validate(sizes, layout, padding, side);
        const auto &exact = layout.at("exact");
        unsigned area = layout["width"].get<unsigned>() * layout["height"].get<unsigned>(),
                 bound = Integer(exact.at("best_bound"));
        Require(exact.at("optimal").is_boolean() && exact.at("model_version") == modelVersion &&
                    exact.at("solver_version") == solverVersion && exact.at("solver") == "ortools-cp-sat" &&
                    bound <= area && Integer(exact.at("objective_area")) == area &&
                    exact.at("cache_key") == key,
                "Cache proof metadata mismatch");
        double gap = exact.at("gap");
        Require(std::isfinite(gap) && std::abs(gap - (area ? double(area - bound) / area : 0.0)) <= 1e-12,
                "Cache gap mismatch");
        auto status = exact.at("status").get<std::string>();
        if (exact.at("optimal").get<bool>())
        {
            Require((status == "OPTIMAL" || status == "LOWER_BOUND_PROVEN") && bound == area,
                    "Cache optimum is not proven");
            if (status == "LOWER_BOUND_PROVEN")
                Require(LowerBound(sizes, padding, side, area) == area, "Cache lower bound is not proven");
        }
        else
            Require(status == "FEASIBLE" || status == "UNKNOWN", "Invalid unproven cache status");
        if (exact.contains("search_complete"))
            Require(exact["search_complete"].is_boolean(), "Invalid completion marker");
        return layout;
    }
    catch (const std::exception &)
    {
        return nullptr;
    }
}
unsigned long long Read64(Reader &reader)
{
    auto low = reader.Word();
    return low | (static_cast<unsigned long long>(reader.Word()) << 32);
}
} // namespace

Json NativeLayouts(const fs::path &compiler, const std::vector<Json> &sizes, const fs::path &work,
                   unsigned maxSide)
{
    Bytes request;
    Word(request, 0x314b5041);
    Word(request, 1);
    Word(request, unsigned(sizes.size()));
    for (auto &batch : sizes)
    {
        Word(request, 2);
        Word(request, unsigned(batch.size()));
        for (auto &pair : batch)
        {
            Word(request, Integer(pair[0]));
            Word(request, Integer(pair[1]));
        }
    }
    Publish(work / "atlas-request.bin", request);
    Run(compiler, {"pack-atlas-layout", "--input", (work / "atlas-request.bin").string(), "--output",
                   (work / "atlas-response.bin").string(), "--max-side", std::to_string(maxSide)});
    auto bytes = Read(work / "atlas-response.bin");
    Reader reader(bytes);
    Require(reader.Word() == 0x314c5041 && reader.Word() == 1 && reader.Word() == sizes.size(),
            "Invalid native atlas response");
    Json result = Json::array();
    for (auto &batch : sizes)
    {
        auto w = reader.Word(), h = reader.Word(), count = reader.Word(), attempts = reader.Word();
        auto baseline = Read64(reader), micros = Read64(reader);
        Require(count == batch.size(), "Native atlas region count differs");
        Json regions = Json::array();
        for (auto &pair : batch)
        {
            auto x = reader.Word(), y = reader.Word();
            regions.push_back({x, y, pair[0], pair[1]});
        }
        Json layout = {{"width", w},
                       {"height", h},
                       {"regions", regions},
                       {"shelf_area", baseline},
                       {"search_attempts", attempts},
                       {"search_microseconds", micros},
                       {"method", "native-maxrects"}};
        Validate(batch, layout, 2, maxSide);
        Require(!baseline || 1ULL * w * h <= baseline, "Native layout regresses shelf packing");
        result.push_back(layout);
    }
    reader.Finish();
    return result;
}

Json ExactLayout(const Json &sizes, const Json &native, const AtlasSettings &settings,
                 const fs::path &directory, const fs::path &seed)
{
    using namespace operations_research;
    using namespace operations_research::sat;
    Validate(sizes, native, settings.padding, settings.maxSide);
    if(settings.fast)
    {
        auto layout=native;
        const auto area=1ULL*Integer(layout["width"])*Integer(layout["height"]);
        layout["exact"]={{"solver","native-maxrects"},{"status","HEURISTIC"},
            {"optimal",false},{"objective_area",area},{"best_bound",nullptr},{"gap",nullptr}};
        return layout;
    }
    fs::create_directories(directory);
    Json identity = {{"format", schema},
                     {"model_version", modelVersion},
                     {"sizes", sizes},
                     {"padding", settings.padding},
                     {"max_side", settings.maxSide},
                     {"solver_version", solverVersion}};
    auto key = Hash(Canonical(identity, true));
    auto path = directory / (key + ".json");
    if (!fs::exists(path) && fs::exists(seed / (key + ".json")))
    {
        auto candidate = Cache(seed / (key + ".json"), key, sizes, settings.padding, settings.maxSide);
        if (!candidate.is_null())
        {
            const auto &exact = candidate["exact"];
            if (exact["optimal"].get<bool>() || exact.value("search_complete", false))
                fs::copy_file(seed / (key + ".json"), path, fs::copy_options::skip_existing);
        }
    }
    auto started = Clock::now();
    auto elapsed = [&] { return std::chrono::duration<double>(Clock::now() - started).count(); };
    if (!fs::exists(path) && settings.maxSide < 16384)
    {
        // Restricting the feasible set preserves a proven optimum and valid
        // lower bounds if the placement still fits. Completed bounded searches
        // can also retain their spent budget and FEASIBLE/UNKNOWN status. Never
        // promote an unproven result or imply it was searched in the new domain.
        auto previousIdentity = identity;
        previousIdentity["max_side"] = 16384;
        const auto previousKey = Hash(Canonical(previousIdentity, true));
        for (const auto &source : {directory, seed})
        {
            auto previous =
                Cache(source / (previousKey + ".json"), previousKey, sizes, settings.padding, 16384);
            if (previous.is_null())
                continue;
            const auto &proof = previous["exact"];
            const bool bounded = !settings.requireOptimal && settings.seconds > 0 &&
                                 proof.value("search_complete", false) &&
                                 proof["solve_budget_seconds"].is_number() &&
                                 proof["solve_budget_seconds"].get<double>() >= settings.seconds &&
                                 Quality(previous) <= Quality(native);
            if (!proof["optimal"].get<bool>() && !bounded)
                continue;
            try
            {
                Validate(sizes, previous, settings.padding, settings.maxSide);
            }
            catch (const std::exception &)
            {
                continue;
            }
            previous["exact"]["cache_key"] = key;
            previous["exact"]["restricted_from_cache_key"] = previousKey;
            previous["exact"]["search_domain_max_side"] = 16384;
            Save(path, key, previous);
            break;
        }
    }
    auto cached = Cache(path, key, sizes, settings.padding, settings.maxSide);
    auto initial = native;
    // All former solver revisions remain valid geometry hints. Only exact
    // matching model/version/digest records may supply proof or completion.
    for (auto &e : fs::directory_iterator(directory))
        if (e.is_regular_file() && e.path().extension() == ".json")
            try
            {
                auto candidate = ReadJson(e.path()).at("layout");
                Validate(sizes, candidate, settings.padding, settings.maxSide);
                if (Quality(candidate) < Quality(initial))
                    for (auto field : {"width", "height", "regions"})
                        initial[field] = candidate[field];
            }
            catch (const std::exception &)
            {
            }
    auto upper = unsigned(Quality(initial)[0]),
         lower = LowerBound(sizes, settings.padding, settings.maxSide, upper);
    bool complete = false;
    if (!cached.is_null())
    {
        const auto &exact = cached["exact"];
        Require(!exact["optimal"].get<bool>() || Integer(exact["objective_area"]) <= upper,
                "Cached proof contradicts better legal layout");
        complete = !settings.requireOptimal && settings.seconds > 0 &&
                   exact.value("search_complete", false) && exact["solve_budget_seconds"].is_number() &&
                   exact["solve_budget_seconds"].get<double>() >= settings.seconds &&
                   Quality(cached) <= Quality(initial);
        if (exact["optimal"].get<bool>() || complete)
        {
            auto result = native;
            for (auto field : {"width", "height", "regions", "exact"})
                result[field] = cached[field];
            result["exact"]["cache_hit"] = true;
            result["exact"][complete ? "cache_lookup_seconds" : "elapsed_seconds"] = elapsed();
            return result;
        }
    }
    auto result = initial;
    std::string status = "LOWER_BOUND_PROVEN";
    unsigned bound = lower;
    bool optimal = upper == lower;
    if (!optimal)
    {
        CpModelBuilder model;
        unsigned minW = 0, minH = 0;
        for (auto &size : sizes)
        {
            minW = std::max(minW, size[0].get<unsigned>() + 2 * settings.padding);
            minH = std::max(minH, size[1].get<unsigned>() + 2 * settings.padding);
        }
        unsigned maxW = std::min(settings.maxSide, upper / minH),
                 maxH = std::min(settings.maxSide, upper / minW);
        auto width = model.NewIntVar(Domain(minW, maxW)), height = model.NewIntVar(Domain(minH, maxH)),
             area = model.NewIntVar(Domain(lower, upper));
        model.AddMultiplicationEquality(area, {width, height});
        auto overlap = model.AddNoOverlap2D();
        std::vector<IntVar> xs, ys;
        std::vector<LinearExpr> right, bottom;
        std::map<std::pair<unsigned, unsigned>, std::vector<unsigned>> equal;
        std::vector<std::pair<int, int>> hints;
        for (std::size_t i = 0; i < sizes.size(); ++i)
        {
            unsigned w = sizes[i][0].get<unsigned>() + 2 * settings.padding,
                     h = sizes[i][1].get<unsigned>() + 2 * settings.padding;
            auto x = model.NewIntVar(Domain(0, maxW - w)), y = model.NewIntVar(Domain(0, maxH - h));
            model.AddLessOrEqual(x + w, width);
            model.AddLessOrEqual(y + h, height);
            overlap.AddRectangle(model.NewFixedSizeIntervalVar(x, w), model.NewFixedSizeIntervalVar(y, h));
            xs.push_back(x);
            ys.push_back(y);
            right.push_back(x + w);
            bottom.push_back(y + h);
            equal[{w, h}].push_back(unsigned(i));
            hints.emplace_back(initial["regions"][i][0].get<int>() - settings.padding,
                               initial["regions"][i][1].get<int>() - settings.padding);
        }
        model.AddMaxEquality(width, right);
        model.AddMaxEquality(height, bottom);
        std::vector<LinearExpr> xExpr(xs.begin(), xs.end()), yExpr(ys.begin(), ys.end());
        model.AddMinEquality(0, xExpr);
        model.AddMinEquality(0, yExpr);
        model.Minimize(area);
        for (auto &[size, indices] : equal)
        {
            std::vector<std::pair<int, int>> points;
            for (auto i : indices)
                points.push_back(hints[i]);
            std::sort(points.begin(), points.end());
            for (std::size_t i = 0; i < indices.size(); ++i)
                hints[indices[i]] = points[i];
            for (std::size_t i = 1; i < indices.size(); ++i)
            {
                auto before = indices[i - 1], after = indices[i];
                model.AddLessOrEqual(xs[before] * (maxH + 1) + ys[before],
                                     xs[after] * (maxH + 1) + ys[after]);
            }
        }
        model.AddHint(width, initial["width"].get<int>());
        model.AddHint(height, initial["height"].get<int>());
        model.AddHint(area, upper);
        for (std::size_t i = 0; i < xs.size(); ++i)
        {
            model.AddHint(xs[i], hints[i].first);
            model.AddHint(ys[i], hints[i].second);
        }
        SatParameters parameters;
        parameters.set_num_search_workers(settings.workers);
        parameters.set_random_seed(0);
        parameters.set_randomize_search(false);
        parameters.set_absolute_gap_limit(0);
        parameters.set_relative_gap_limit(0);
        if (settings.seconds > 0)
            parameters.set_max_time_in_seconds(settings.seconds);
        Model solver;
        solver.Add(NewSatParameters(parameters));
        auto candidate = [&](const CpSolverResponse &response) {
            Json row = {{"width", SolutionIntegerValue(response, width)},
                        {"height", SolutionIntegerValue(response, height)},
                        {"regions", Json::array()}};
            for (std::size_t i = 0; i < xs.size(); ++i)
                row["regions"].push_back({SolutionIntegerValue(response, xs[i]) + settings.padding,
                                          SolutionIntegerValue(response, ys[i]) + settings.padding,
                                          sizes[i][0], sizes[i][1]});
            return row;
        };
        solver.Add(NewFeasibleSolutionObserver([&](const CpSolverResponse &response) {
            auto row = candidate(response);
            Validate(sizes, row, settings.padding, settings.maxSide);
            if (Quality(row) < Quality(result))
                for (auto field : {"width", "height", "regions"})
                    result[field] = row[field];
            auto checkpoint = result;
            auto areaNow = unsigned(Quality(result)[0]);
            auto best =
                std::clamp(std::floor(response.best_objective_bound()), double(lower), double(areaNow));
            checkpoint["exact"] =
                Metadata("FEASIBLE", unsigned(best), checkpoint, false, elapsed(), key, false);
            Save(path, key, checkpoint);
        }));
        auto response = SolveCpModel(model.Build(), &solver);
        status = CpSolverStatus_Name(response.status());
        Require(response.status() != CpSolverStatus::MODEL_INVALID &&
                    response.status() != CpSolverStatus::INFEASIBLE,
                "Exact solver rejected known legal incumbent: " + status);
        bound =
            unsigned(std::clamp(std::floor(response.best_objective_bound()), double(lower), double(upper)));
        if (response.status() == CpSolverStatus::FEASIBLE || response.status() == CpSolverStatus::OPTIMAL)
        {
            auto row = candidate(response);
            Validate(sizes, row, settings.padding, settings.maxSide);
            if (Quality(row) < Quality(result))
                for (auto field : {"width", "height", "regions"})
                    result[field] = row[field];
        }
        optimal = response.status() == CpSolverStatus::OPTIMAL;
        if (optimal)
        {
            Require(SolutionIntegerValue(response, area) == Quality(result)[0],
                    "Solver optimum contradicts incumbent");
            bound = unsigned(Quality(result)[0]);
        }
    }
    result["exact"] = Metadata(status, bound, result, optimal, elapsed(), key, !cached.is_null());
    result["exact"]["search_complete"] = true;
    result["exact"]["solve_budget_seconds"] = settings.seconds > 0 ? Json(settings.seconds) : Json(nullptr);
    Save(path, key, result);
    Validate(sizes, result, settings.padding, settings.maxSide);
    Require(Quality(result) <= Quality(native), "Exact result regresses native layout");
    Require(!settings.requireOptimal || optimal,
            "Atlas optimum remains unproven; retained legal checkpoint; publication refused");
    return result;
}
} // namespace pvz::build
