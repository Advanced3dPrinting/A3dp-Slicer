#include "catch2/catch.hpp"

#include "slic3r/Utils/PresetUpdater.hpp"

#include <string>

using namespace Slic3r;

// Regression test for the beta3 startup SEGV (t_e9421c6f):
// VendorSync::best is a raw pointer into available_profiles. sort_available()
// used to leave it dangling across calls, and the next parse_tags() appended
// to the vector (reallocating it) before calling sort_available() again, which
// dereferenced the stale pointer in `best->config_version < version.config_version`
// -> SEGV in Semver::operator< -> binary_comparison.

namespace {
// Build a GitHub /tags-style JSON response (array of release objects).
std::string tags_response(std::initializer_list<std::pair<std::string, bool>> tags_with_commit)
{
    std::string json = "[";
    bool first = true;
    for (const auto &twc : tags_with_commit) {
        if (!first) json += ",";
        first = false;
        json += "{\"name\":\"" + twc.first + "\",\"zipball_url\":\"https://api.github.com/repos/x/zipball/1\"";
        if (twc.second)
            json += ",\"commit\":{\"sha\":\"abc123\",\"url\":\"https://api.github.com/repos/x/commits/abc123\"}";
        json += "}";
    }
    json += "]";
    return json;
}

// Simulate the state load_unused_vendors() leaves behind: one profile derived
// from the vendor ini, with sort_available() having set `best`.
VendorSync vendor_with_installed_profile()
{
    VendorSync vendor;
    vendor.profile.id = "TestVendor";
    vendor.profile.config_version = *Semver::parse("2.3.1");
    vendor.is_installed = true;
    vendor.available_profiles.emplace_back(VendorAvailable{
        vendor.profile.config_version, *Semver::parse("2.7.62.0"),
        /*local_file=*/std::string("/tmp/TestVendor.ini"), /*url_zip=*/std::string(),
        /*commit_sha=*/std::string(), /*commit_url=*/std::string(),
        /*tag=*/vendor.profile.config_version.to_string() + "=" + Semver("2.7.62.0").to_string(),
        /*notes=*/std::string()});
    vendor.sort_available();
    return vendor;
}

bool best_points_into_vector(const VendorSync &vendor)
{
    if (vendor.available_profiles.empty())
        return vendor.best == nullptr;
    return vendor.best >= vendor.available_profiles.data() &&
           vendor.best <  vendor.available_profiles.data() + vendor.available_profiles.size();
}
} // namespace

TEST_CASE("VendorSync: non-semver release tags are skipped gracefully", "[PresetUpdater]") {
    VendorSync vendor = vendor_with_installed_profile();

    const std::string json = tags_response({
        {"2.3.1=2.7.62.0", true},          // already here (matches installed profile) -> no dup
        {"totally-bogus-tag", true},        // no '=' -> skip
        {"=2.7.62.0", true},                // empty config half -> skip
        {"2.3.1=", true},                   // empty slicer half -> skip
        {"bad=chars!=here", true},          // '!' is not a valid semver char -> skip
        {"notsemver=alsonot", true},        // neither half parses -> skip
        {"1.2.3.AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=2.7.62.0", true}, // >50-char slice crashed semver_parse_version before the hardening
        {"2.3.1=2.7.62.0-beta1", true},     // valid -> added
        {"2.3.2=2.7.62.0", false},          // valid but missing commit info -> skip
    });

    // Must not crash; must return without throwing.
    vendor.parse_tags(json);

    // One valid new tag added; every non-semver / commit-less tag skipped.
    REQUIRE(vendor.available_profiles.size() == 2);
    bool found_beta = false;
    for (const VendorAvailable &va : vendor.available_profiles)
        if (va.tag == "2.3.1=2.7.62.0-beta1") found_beta = true;
    REQUIRE(found_beta);
    // Sorted, so the highest slicer version is first.
    REQUIRE(vendor.available_profiles.front().slicer_version == Semver("2.7.62.0"));
    // `best` was recomputed and points into the live vector, not freed memory.
    REQUIRE(best_points_into_vector(vendor));
    REQUIRE(vendor.best != nullptr);
    REQUIRE(vendor.best->config_version == Semver("2.3.1"));
}

TEST_CASE("VendorSync: parse_tags twice does not leave a dangling best pointer", "[PresetUpdater]") {
    VendorSync vendor = vendor_with_installed_profile();

    // First sync: appends a new tag (vector grows 1 -> 2, may reallocate).
    vendor.parse_tags(tags_response({{"2.3.1=2.7.62.0-beta1", true}}));
    REQUIRE(best_points_into_vector(vendor));

    // Second sync: appends again (vector grows further, reallocation very likely).
    vendor.parse_tags(tags_response({{"2.3.2=2.7.62.0", true}}));
    REQUIRE(best_points_into_vector(vendor));
    REQUIRE(vendor.best != nullptr);
    // best is the highest config_version among entries compatible with this slicer.
    REQUIRE(vendor.best->config_version == Semver("2.3.2"));
}

TEST_CASE("Semver: comparison against malformed versions does not segfault", "[Semver]") {
    // A semver_t with counter_size > 0 but a NULL counters array must not crash
    // the comparison operators (hardened in semver.c).
    semver_t bad = {nullptr, 4, nullptr, nullptr};
    semver_t good = {nullptr, 0, nullptr, nullptr}; // zeroed - semver_parse frees ver->counters if set
    REQUIRE(::semver_parse("1.2.3", &good) == 0);
    // Compares by counter_size only; values are deterministic, no crash is the point.
    REQUIRE(::semver_compare(bad, good) == 1);
    REQUIRE(::semver_compare(good, bad) == -1);
    REQUIRE(::semver_compare(bad, bad) == 0);
    ::semver_free(&good);
}
