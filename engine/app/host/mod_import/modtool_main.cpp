// twinsanityevolved-modtool -- the mod importer on the command line (the same library the launcher's Mods page uses):
//
//   twinsanityevolved-modtool import  <artefact> --iso <disc> --mods <dir> [--json]
//   twinsanityevolved-modtool inspect <artefact> --iso <disc> [--json]
//   twinsanityevolved-modtool verify  <mods>/<id> --iso <disc> [--json]
//   twinsanityevolved-modtool detect  <artefact> [--json]
//
// import  turns a Mod Crate (zip or folder), a modified disc image, an xdelta / PPF / BPS patch, a folder of files or one loose
//         file into <dir>/<id>/ (a mod of that id is replaced: the one rule); inspect says what an import would do; verify recomposes a mod folder
//         over the disc through the game's own overlay and checks every file against its mod.json; detect names what a
//         file or folder is. Progress goes to stderr, the result to stdout (one JSON object with --json).
// Exit status: 0 done, 1 refused or failed (the reason on stderr, and in the JSON), 2 a usage error.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "app/host/mod_import/io.h"
#include "app/host/mod_import/json.h"
#include "app/host/mod_import/mod_import.h"
#include "app/host/region_map_make.h"

namespace {

namespace mi = te::modimport;
using mi::json::Value;

const char* const kUsage =
    "usage:\n"
    "  twinsanityevolved-modtool import  <mod> --iso <disc image> --mods <mods folder> [--json]\n"
    "  twinsanityevolved-modtool inspect <mod> --iso <disc image> [--json]\n"
    "  twinsanityevolved-modtool verify  <mods folder>/<id> --iso <disc image> [--json]\n"
    "  twinsanityevolved-modtool detect  <mod> [--json]\n"
    "  twinsanityevolved-modtool region-map --pal <the European disc> --disc <another region's disc> [--out FILE]\n"
    "A mod is a Mod Crate (.zip or folder), a modified disc image, an xdelta, PPF or BPS patch, or a folder of files.\n";

Value Strings(const std::vector<std::string>& v) {
    Value a = Value::Arr();
    for (const std::string& s : v) a.Push(Value::Str(s));
    return a;
}

mi::Progress StderrProgress(bool quiet) {
    mi::Progress p;
    p.report = [quiet, last = std::string(), shown = -1](const std::string& stage, double f) mutable {
        if (quiet) return;
        const int pct = int(f * 100.0 + 0.5);
        if (stage == last && pct == shown) return;
        if (stage != last || pct >= shown + 5 || pct == 100) {
            std::fprintf(stderr, "[modtool] %s %d%%\n", stage.c_str(), pct);
            last = stage;
            shown = pct;
        }
    };
    return p;
}

// The line naming what the artefact is: "<label>: <sentence>"; one loose file's sentence says it all
std::string KindLine(mi::Kind k, const std::string& target, const std::string& why) {
    if (k == mi::Kind::Folder && !mi::io::IsDir(target)) return why;
    return std::string(mi::KindLabel(k)) + ": " + why;
}

int Finish(bool json, const Value& out, bool ok, const std::string& error) {
    if (json) {
        Value o = out;
        o.Set("ok", Value::Bool(ok));
        if (!ok) o.Set("error", Value::Str(error));
        std::fputs(mi::json::Write(o).c_str(), stdout);
    }
    if (!ok) std::fprintf(stderr, "twinsanityevolved-modtool: %s\n", error.c_str());
    return ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2 || !std::strcmp(argv[1], "--help") || !std::strcmp(argv[1], "-h")) {
        std::fputs(kUsage, argc < 2 ? stderr : stdout);
        return argc < 2 ? 2 : 0;
    }
    const std::string command = argv[1];
    if (command == "region-map") return ps2r::app::RegionMapCommand(argc - 1, argv + 1);   // engine/app/host/region_map_make.h
    std::string target, iso, mods;
    bool json = false, quiet = false;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--iso" && i + 1 < argc) iso = argv[++i];
        else if (a == "--mods" && i + 1 < argc) mods = argv[++i];
        else if (a == "--json") json = true;
        else if (a == "--quiet") quiet = true;
        else if (!a.empty() && a[0] == '-') {
            std::fprintf(stderr, "twinsanityevolved-modtool: unknown option %s\n%s", a.c_str(), kUsage);
            return 2;
        } else if (target.empty()) target = a;
        else {
            std::fprintf(stderr, "twinsanityevolved-modtool: one mod at a time (%s)\n%s", a.c_str(), kUsage);
            return 2;
        }
    }
    if (target.empty() || (command != "detect" && iso.empty()) || (command == "import" && mods.empty())) {
        std::fputs(kUsage, stderr);
        return 2;
    }
    const mi::Progress progress = StderrProgress(quiet || json);
    Value out = Value::Obj();
    out.Set("command", Value::Str(command));
    out.Set("target", Value::Str(target));

    if (command == "detect") {
        std::string why;
        const mi::Kind k = mi::Detect(target, &why);
        out.Set("kind", Value::Str(mi::SourceKindName(k)));
        out.Set("label", Value::Str(mi::KindLabel(k)));
        out.Set("detected", Value::Str(why));
        if (!json) std::printf("%s\n", KindLine(k, target, why).c_str());
        return Finish(json, out, k != mi::Kind::Unknown, k == mi::Kind::Unknown ? why : "");
    }

    if (command == "import") {
        mi::Report r;
        std::string error;
        const bool ok = mi::Import(target, iso, mods, progress, &r, &error);
        out.Set("id", Value::Str(r.id));
        out.Set("name", Value::Str(r.name));
        out.Set("mod_json", Value::Str(r.mod_json_path));
        out.Set("files_changed", Strings(r.files_changed));
        out.Set("files_added", Strings(r.files_added));
        out.Set("warnings", Strings(r.warnings));
        out.Set("bytes_written", Value::Num(r.bytes_written));
        out.Set("code_changes", Strings(r.code_changes));
        out.Set("code_changes_unsupported", Value::Bool(r.code_changes_unsupported));
        out.Set("replaced", Value::Bool(r.replaced));
        if (ok && !json) {
            std::printf("imported %s as %s (%s)%s\n", r.name.c_str(), r.id.c_str(), r.mod_json_path.c_str(),
                        r.replaced ? ", replacing the mod of that id" : "");
            for (const std::string& f : r.files_changed) std::printf("  changed  %s\n", f.c_str());
            for (const std::string& f : r.files_added) std::printf("  added    %s\n", f.c_str());
            if (r.code_changes_unsupported) {
                std::printf("  code changes not supported (%zu function(s)): the mod's data changes apply, its code changes cannot\n",
                            r.code_changes.size());
                for (const std::string& f : r.code_changes) std::printf("    %s\n", f.c_str());
            }
            for (const std::string& w : r.warnings) std::printf("  note: %s\n", w.c_str());
            std::printf("  %llu bytes written\n", static_cast<unsigned long long>(r.bytes_written));
        }
        return Finish(json, out, ok, error);
    }

    if (command == "inspect") {
        mi::Inspection in;
        std::string error;
        const bool ok = mi::Inspect(target, iso, progress, &in, &error);
        out.Set("kind", Value::Str(mi::SourceKindName(in.kind)));
        out.Set("label", Value::Str(mi::KindLabel(in.kind)));
        out.Set("detected", Value::Str(in.detected));
        out.Set("name", Value::Str(in.name));
        out.Set("author", Value::Str(in.author));
        out.Set("version", Value::Str(in.version));
        out.Set("description", Value::Str(in.description));
        out.Set("notes", Strings(in.notes));
        out.Set("importable", Value::Bool(in.importable));
        out.Set("refusal", Value::Str(in.refusal));
        out.Set("files_changed", Strings(in.files_changed));
        out.Set("files_added", Strings(in.files_added));
        out.Set("files_removed", Strings(in.files_removed));
        out.Set("archive_entries_changed", Strings(in.archive_entries_changed));
        out.Set("archive_entries_added", Strings(in.archive_entries_added));
        out.Set("code_changes", Strings(in.code_changes));
        Value data = Value::Obj();
        for (const auto& kv : in.data_changes) data.Set(kv.first, Value::Num(kv.second));
        out.Set("data_changes", std::move(data));
        out.Set("warnings", Strings(in.warnings));
        out.Set("needs_apply", Value::Bool(in.needs_apply));
        out.Set("patch_format", Value::Str(in.patch_format));
        out.Set("temporary_bytes", Value::Num(in.temporary_bytes));
        if (ok && !json) {
            std::printf("%s\n", KindLine(in.kind, target, in.detected).c_str());
            if (!in.name.empty()) std::printf("  name     %s\n", in.name.c_str());
            if (!in.author.empty()) std::printf("  author   %s\n", in.author.c_str());
            if (!in.version.empty()) std::printf("  version  %s\n", in.version.c_str());
            for (const std::string& n : in.notes) std::printf("  says     %s\n", n.c_str());
            if (!in.patch_format.empty()) std::printf("  format   %s\n", in.patch_format.c_str());
            for (const std::string& f : in.files_changed) std::printf("  changes  %s\n", f.c_str());
            for (const std::string& f : in.files_added) std::printf("  adds     %s\n", f.c_str());
            for (const std::string& f : in.files_removed) std::printf("  lacks    %s\n", f.c_str());
            for (const std::string& f : in.archive_entries_changed) std::printf("  archive  changes %s\n", f.c_str());
            for (const std::string& f : in.archive_entries_added) std::printf("  archive  adds %s\n", f.c_str());
            for (const auto& kv : in.data_changes) std::printf("  program  %s: %llu byte(s) changed\n", kv.first.c_str(), static_cast<unsigned long long>(kv.second));
            for (const std::string& f : in.code_changes) std::printf("  code     %s (not supported)\n", f.c_str());
            if (in.needs_apply) std::printf("  its files are known once it is applied (import needs %llu bytes of temporary space)\n",
                                            static_cast<unsigned long long>(in.temporary_bytes));
            for (const std::string& w : in.warnings) std::printf("  note: %s\n", w.c_str());
            std::printf("  %s\n", in.importable ? "it can be imported" : ("it cannot be imported: " + in.refusal).c_str());
        }
        return Finish(json, out, ok && in.importable, ok ? in.refusal : error);
    }

    if (command == "verify") {
        mi::Verification v;
        std::string error;
        const bool ok = mi::Verify(target, iso, progress, &v, &error);
        out.Set("id", Value::Str(v.id));
        Value rows = Value::Arr();
        for (const mi::VerifyRow& row : v.files) {
            Value o = Value::Obj();
            o.Set("path", Value::Str(row.path));
            o.Set("form", Value::Str(row.form));
            o.Set("bytes", Value::Num(row.bytes));
            o.Set("recorded_bytes", Value::Num(row.recorded_bytes));
            o.Set("sha256", Value::Str(row.sha256));
            o.Set("recorded_sha256", Value::Str(row.recorded_sha256));
            o.Set("ok", Value::Bool(row.ok));
            o.Set("note", Value::Str(row.note));
            rows.Push(std::move(o));
        }
        out.Set("files", std::move(rows));
        out.Set("problems", Strings(v.problems));
        if (ok && !json) {
            for (const mi::VerifyRow& row : v.files)
                std::printf("  %-4s %-28s %-8s %12llu bytes  %s%s%s\n", row.ok ? "ok" : "BAD", row.path.c_str(), row.form.c_str(),
                            static_cast<unsigned long long>(row.bytes), row.sha256.c_str(), row.note.empty() ? "" : "  ", row.note.c_str());
            for (const std::string& p : v.problems) std::printf("  problem: %s\n", p.c_str());
            std::printf("%s %s\n", v.id.c_str(), v.ok ? "verifies: every file composes to what its mod.json records" : "does NOT verify");
        }
        return Finish(json, out, ok && v.ok, ok ? (v.problems.empty() ? std::string("it does not verify") : v.problems.front()) : error);
    }

    std::fprintf(stderr, "twinsanityevolved-modtool: unknown command %s\n%s", command.c_str(), kUsage);
    return 2;
}
