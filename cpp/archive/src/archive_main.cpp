#include "rapid/native.hpp"
#include <argon2.h>
#include <csignal>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <set>
#include <sstream>
#include <unistd.h>

using namespace rapid::native;
namespace {
struct Failure {
  int status;
  std::string message;
};
void need(bool condition, int status, const char *message) {
  if (!condition)
    throw Failure{status, message};
}
std::string secret(const std::string &name, const std::string &fallback) {
  auto value = env(name);
  if (value.empty())
    value = read_file(env(name + "_FILE", fallback));
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.back())))
    value.pop_back();
  if (value.empty())
    throw std::runtime_error("missing archive secret");
  return value;
}
bool verify(const std::string &hash, const std::string &value) {
  return value.size() <= 1024 &&
         argon2_verify(hash.c_str(), value.data(), value.size(), Argon2_id) ==
             ARGON2_OK;
}
bool safe_relative(const std::string &value) {
  if (value.empty() || value.size() > 512 || value.front() == '/' ||
      value.find('\\') != std::string::npos ||
      value.find(':') != std::string::npos)
    return false;
  std::istringstream in(value);
  std::string part;
  while (std::getline(in, part, '/')) {
    if (part.empty() || part == "." || part == "..")
      return false;
    for (auto ch : part)
      if (static_cast<unsigned char>(ch) < 32)
        return false;
  }
  return value.back() != '/';
}
std::string archive_path(const Json &item, int ordinal) {
  auto role = string(item, "role");
  if (role == "full_ld" || role == "full" || role == "full_session" ||
      role == "session_log" || role == "full_log")
    return "full-session.ld";
  if (role == "ldx" || role == "index" || role == "full_session_index")
    return "full-session.ldx";
  if (role == "lap_ld" || role == "lap" || role == "lap_log" ||
      role == "completed_lap")
    return "laps/lap-" +
           std::to_string(int(number(item, "lap_number", ordinal))) + ".ld";
  return "artifacts/" + std::to_string(ordinal) + "-" +
         safe_name(fs::path(string(item, "relative_path")).filename().string());
}
struct Archive {
  fs::path root, staging, committed;
  Database db;
  std::mutex mutex;
  std::string owner, owner_hash, ingest_hash;
  std::uint64_t maximum;
  Archive(fs::path data, fs::path database, fs::path stage, fs::path final,
          std::string username, std::string password, std::string token,
          std::uint64_t limit, const fs::path &assets)
      : root(std::move(data)), staging(std::move(stage)),
        committed(std::move(final)), db(database), owner(std::move(username)),
        owner_hash(std::move(password)), ingest_hash(std::move(token)),
        maximum(limit) {
    fs::create_directories(staging);
    fs::create_directories(committed);
    auto schema = read_file(assets / "archive-schema.sql");
    std::istringstream input(schema);
    std::string sql;
    while (std::getline(input, sql, ';'))
      if (sql.find_first_not_of(" \r\n\t") != std::string::npos)
        db.exec(sql);
  }
  bool auth(const Request &request, bool ingest) {
    auto it = request.headers.find("authorization");
    if (it == request.headers.end())
      return false;
    const auto &value = it->second;
    if (ingest)
      return value.starts_with("Bearer ") &&
             verify(ingest_hash, value.substr(7));
    if (!value.starts_with("Basic "))
      return false;
    auto encoded = value.substr(6);
    if (encoded.size() > 2048)
      return false;
    std::string decoded(encoded.size(), '\0');
    auto length =
        EVP_DecodeBlock(reinterpret_cast<unsigned char *>(decoded.data()),
                        reinterpret_cast<const unsigned char *>(encoded.data()),
                        encoded.size());
    if (length < 0)
      return false;
    for (auto i = encoded.rbegin(); i != encoded.rend() && *i == '='; ++i)
      --length;
    decoded.resize(length);
    auto colon = decoded.find(':');
    if (colon == std::string::npos)
      return false;
    auto username = decoded.substr(0, colon);
    return username.size() == owner.size() &&
           CRYPTO_memcmp(username.data(), owner.data(), owner.size()) == 0 &&
           verify(owner_hash, decoded.substr(colon + 1));
  }
  Json row(const std::string &table, const std::string &id) {
    auto rows = db.query("SELECT * FROM " + table + " WHERE id=?", {id});
    need(!rows.empty(), 404, "unknown resource");
    return rows[0];
  }
  Response handle(const Request &request) {
    try {
      auto target = request.target.substr(0, request.target.find('?'));
      if (target == "/healthz" && request.method == "GET")
        return {200, "{\"status\":\"ok\",\"runtime\":\"cpp\"}"};
      bool list = target == "/v1/sessions" && request.method == "GET";
      if (!auth(request, !list))
        return {401,
                "{\"detail\":\"invalid credentials\"}",
                "application/json",
                {{"WWW-Authenticate",
                  list ? "Basic realm=\"raPId archive\"" : "Bearer"}}};
      std::lock_guard lock(mutex);
      if (list)
        return {200,
                db.query("SELECT "
                         "id,source_session_id,simulator,track,started_at,"
                         "committed_at,archive_relpath FROM sessions WHERE "
                         "status='committed' ORDER BY committed_at DESC")
                    .dump()};
      if (target == "/v1/sessions" && request.method == "POST")
        return declare(Json::parse(request.body));
      const std::string artifacts = "/v1/artifacts/",
                        sessions = "/v1/sessions/";
      if (target.starts_with(artifacts)) {
        auto id = target.substr(artifacts.size());
        need(id.size() == 32 &&
                 id.find_first_not_of("0123456789abcdef") == std::string::npos,
             404, "unknown artifact");
        auto item = row("artifacts", id);
        auto offset = item["upload_offset"].get<std::uint64_t>(),
             size = item["expected_size"].get<std::uint64_t>();
        if (request.method == "HEAD")
          return {200,
                  "",
                  "application/octet-stream",
                  {{"Upload-Offset", std::to_string(offset)},
                   {"Upload-Length", std::to_string(size)}}};
        if (request.method == "PATCH") {
          need(request.headers.contains("upload-offset"), 400,
               "Upload-Offset required");
          std::size_t used = 0;
          auto given = std::stoull(request.headers.at("upload-offset"), &used);
          need(used == request.headers.at("upload-offset").size(), 400,
               "invalid offset");
          if (given != offset)
            return {409,
                    "",
                    "application/json",
                    {{"Upload-Offset", std::to_string(offset)}}};
          need(request.body.size() <= size - offset, 413,
               "artifact exceeds declaration");
          need(item["status"] != "committed", 409,
               "artifact already committed");
          auto path = staging / string(item, "session_id") /
                      string(item, "staging_filename");
          fs::create_directories(path.parent_path());
          int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
          if (fd < 0)
            throw std::runtime_error("cannot open upload");
          struct Close {
            int fd;
            ~Close() { ::close(fd); }
          } close{fd};
          auto actual = fs::file_size(path);
          need(actual >= offset, 409,
               "staged artifact shorter than committed offset");
          // Truncate uncommitted bytes left by a crash, then write at the
          // durable database offset. Never append to an unverified filesystem
          // length.
          if (ftruncate(fd, offset) || lseek(fd, offset, SEEK_SET) < 0)
            throw std::runtime_error("upload seek failed");
          std::size_t written = 0;
          while (written < request.body.size()) {
            auto n = ::write(fd, request.body.data() + written,
                             request.body.size() - written);
            if (n <= 0)
              throw std::runtime_error("upload write failed");
            written += n;
          }
          if (fsync(fd))
            throw std::runtime_error("upload sync failed");
          sync_file(path.parent_path());
          offset += written;
          db.exec("UPDATE artifacts SET "
                  "upload_started=1,upload_offset=?,status=? WHERE id=?",
                  {offset, offset == size ? "uploaded" : "uploading", id});
          return {204,
                  "",
                  "application/json",
                  {{"Upload-Offset", std::to_string(offset)}}};
        }
      }
      if (target.starts_with(sessions) && target.ends_with("/commit") &&
          request.method == "POST") {
        auto id =
            target.substr(sessions.size(), target.size() - sessions.size() - 7);
        need(id.size() == 32 &&
                 id.find_first_not_of("0123456789abcdef") == std::string::npos,
             404, "unknown session");
        return commit(id);
      }
      return {404, "{\"detail\":\"not found\"}"};
    } catch (const Failure &f) {
      return {f.status, Json{{"detail", f.message}}.dump()};
    }
  }
  Response declare(const Json &manifest) {
    need(manifest.is_object(), 400, "manifest object required");
    auto source = string(manifest, "session_id"),
         simulator = string(manifest, "simulator"),
         track = string(manifest, "track");
    need(!source.empty() && source.size() <= 128 && !simulator.empty() &&
             simulator.size() <= 128 && track.size() <= 256,
         400, "invalid session metadata");
    need(manifest.contains("artifacts") && manifest["artifacts"].is_array() &&
             !manifest["artifacts"].empty() &&
             manifest["artifacts"].size() <= 10000,
         400, "invalid artifacts");
    std::set<std::string> paths, ids, generated;
    std::uint64_t total = 0;
    int ordinal = 0;
    for (const auto &item : manifest["artifacts"]) {
      auto rel = string(item, "relative_path"), hash = string(item, "sha256"),
           id = string(item, "id");
      need(item.is_object() && safe_relative(rel) && hash.size() == 64 &&
               hash.find_first_not_of("0123456789abcdef") == std::string::npos,
           400, "invalid artifact declaration");
      need(item.contains("size") && item["size"].is_number_integer() &&
               number(item, "size") >= 0,
           400, "invalid artifact size");
      auto size = item["size"].get<std::uint64_t>();
      need(size <= maximum && total <= maximum * 4 - size, 413,
           "archive size limit");
      total += size;
      for (auto &ch : rel)
        ch = std::tolower(static_cast<unsigned char>(ch));
      need(paths.insert(rel).second && (id.empty() || ids.insert(id).second) &&
               generated.insert(archive_path(item, ++ordinal)).second,
           400, "duplicate artifact declaration");
    }
    auto digest = hash_text(manifest.dump());
    auto existing =
        db.query("SELECT * FROM sessions WHERE source_session_id=?", {source});
    std::string id;
    if (!existing.empty()) {
      need(existing[0]["manifest_sha256"] == digest, 409,
           "session already declared with different manifest");
      id = existing[0]["id"];
    } else {
      id = unique_id();
      auto date = string(manifest, "started_at", now()).substr(0, 10);
      auto relative = safe_name(date) + "/" + safe_name(simulator) + "/" +
                      safe_name(track) + "/" + id;
      db.exec("BEGIN IMMEDIATE");
      try {
        db.exec("INSERT INTO sessions VALUES(?,?,?,?,?,?,?,?,'staging',?,NULL)",
                {id, source, digest, manifest.dump(), simulator, track,
                 manifest.value("started_at", Json()), relative, now()});
        ordinal = 0;
        for (const auto &item : manifest["artifacts"]) {
          auto artifact = unique_id();
          db.exec(
              "INSERT INTO "
              "artifacts(id,session_id,source_artifact_id,role,source_relative_"
              "path,archive_relpath,expected_size,expected_sha256,media_type,"
              "staging_filename,status) VALUES(?,?,?,?,?,?,?,?,?,?,?)",
              {artifact, id, item.value("id", Json()), string(item, "role"),
               item.at("relative_path"), archive_path(item, ++ordinal),
               item.at("size"), item.at("sha256"), "application/octet-stream",
               artifact + ".part",
               number(item, "size") == 0 ? "uploaded" : "declared"});
        }
        db.exec("COMMIT");
      } catch (...) {
        db.exec("ROLLBACK");
        throw;
      }
    }
    Json artifacts = Json::array();
    for (const auto &a : db.query(
             "SELECT * FROM artifacts WHERE session_id=? ORDER BY rowid", {id}))
      artifacts.push_back(
          {{"id", a["id"]},
           {"size", a["expected_size"]},
           {"offset", a["upload_offset"]},
           {"status", a["status"]},
           {"url", "/v1/artifacts/" + a["id"].get<std::string>()}});
    return {201, Json{{"session_id", id},
                      {"commit_url", "/v1/sessions/" + id + "/commit"},
                      {"artifacts", artifacts}}
                     .dump()};
  }
  Response commit(const std::string &id) {
    auto session = row("sessions", id);
    auto relative = string(session, "archive_relpath");
    need(safe_relative(relative), 409, "invalid stored archive path");
    auto destination = committed / relative;
    auto items = db.query(
        "SELECT * FROM artifacts WHERE session_id=? ORDER BY rowid", {id});
    auto validate = [&](const fs::path &base, bool published) {
      for (const auto &item : items) {
        auto rel =
            string(item, published ? "archive_relpath" : "staging_filename");
        need(safe_relative(rel), 409, "invalid stored artifact path");
        auto path = base / rel;
        auto size = item["expected_size"].get<std::uint64_t>();
        if (!published && size == 0 && !fs::exists(path))
          atomic_file(path, "");
        need(fs::is_regular_file(path) && fs::file_size(path) == size &&
                 hash_file(path) == string(item, "expected_sha256"),
             409, "all artifacts must be complete and verified");
      }
    };
    if (!fs::exists(destination)) {
      validate(staging / id, false);
      auto publish = committed / (".publishing-" + unique_id());
      fs::create_directories(publish);
      atomic_file(publish / "manifest.json",
                  string(session, "manifest_json") + "\n");
      for (const auto &item : items) {
        auto target = publish / string(item, "archive_relpath");
        fs::create_directories(target.parent_path());
        fs::copy_file(staging / id / string(item, "staging_filename"), target);
        sync_file(target);
        sync_file(target.parent_path());
      }
      sync_file(publish);
      fs::create_directories(destination.parent_path());
      fs::rename(publish, destination);
      sync_file(destination.parent_path());
    } else {
      validate(destination, true);
      need(hash_text(
               Json::parse(read_file(destination / "manifest.json")).dump()) ==
               string(session, "manifest_sha256"),
           409, "published manifest mismatch");
    }
    db.exec("BEGIN IMMEDIATE");
    try {
      db.exec(
          "UPDATE sessions SET "
          "status='committed',committed_at=COALESCE(committed_at,?) WHERE id=?",
          {now(), id});
      db.exec("UPDATE artifacts SET status='committed' WHERE session_id=?",
              {id});
      db.exec("COMMIT");
    } catch (...) {
      db.exec("ROLLBACK");
      throw;
    }
    return {
        200,
        Json{{"session_id", id}, {"status", "committed"}, {"path", relative}}
            .dump()};
  }
};
void self_test(const fs::path &assets) {
  auto root =
      fs::temp_directory_path() / ("rapid-archive-tests-" + unique_id());
  fs::create_directories(root);
  std::string token = "native-integration-test";
  char hash[256];
  std::string salt = "native-test-salt-2026";
  if (argon2id_hash_encoded(2, 8192, 1, token.data(), token.size(), salt.data(),
                            salt.size(), 32, hash, sizeof hash) != ARGON2_OK)
    throw std::runtime_error("test hash failed");
  Archive archive(root, root / "archive.db", root / "staging",
                  root / "committed", "owner", hash, hash, 1024 * 1024, assets);
  auto call = [&](std::string method, std::string target, std::string body = "",
                  std::string offset = "") {
    Request req{method, target, body, {{"authorization", "Bearer " + token}}};
    if (!offset.empty())
      req.headers["upload-offset"] = offset;
    return archive.handle(req);
  };
  auto check = [](bool condition, const char *text) {
    if (!condition)
      throw std::runtime_error(text);
  };
  check(archive.handle({"POST", "/v1/sessions", "{}", {}}).status == 401,
        "missing token rejected");
  Json manifest = {
      {"session_id", "archive-native-test"},
      {"simulator", "ACC"},
      {"track", ""},
      {"artifacts", Json::array({{{"id", "full-ld"},
                                  {"role", "full_ld"},
                                  {"relative_path", "full-session.ld"},
                                  {"size", 6},
                                  {"sha256", hash_text("abcdef")}}})}};
  auto declared = call("POST", "/v1/sessions", manifest.dump());
  check(declared.status == 201, "declare");
  auto value = Json::parse(declared.body);
  auto url = value["artifacts"][0]["url"].get<std::string>(),
       commit = value["commit_url"].get<std::string>();
  check(call("POST", "/v1/sessions", manifest.dump()).body == declared.body,
        "idempotent declaration");
  check(call("PATCH", url, "abc", "0").status == 204, "initial chunk");
  check(call("PATCH", url, "abc", "0").status == 409, "offset mismatch");
  auto partial = root / "staging" / value["session_id"].get<std::string>() /
                 (value["artifacts"][0]["id"].get<std::string>() + ".part");
  {
    std::ofstream append(partial, std::ios::app);
    append << "uncommitted crash bytes";
  }
  check(call("PATCH", url, "def", "3").status == 204,
        "resume overwrites uncommitted tail");
  check(read_file(partial) == "abcdef", "no duplicate appended bytes");
  auto result = call("POST", commit);
  check(result.status == 200, "verified commit");
  auto published =
      root / "committed" / Json::parse(result.body)["path"].get<std::string>();
  check(read_file(published / "full-session.ld") == "abcdef",
        "producer role layout");
  // Simulate a crash after rename but before database commit.
  archive.db.exec("UPDATE sessions SET status='staging',committed_at=NULL");
  archive.db.exec("UPDATE artifacts SET status='uploaded'");
  check(call("POST", commit).status == 200, "commit recovery after rename");
  check(call("PATCH", url, "", "6").status == 409,
        "committed artifacts immutable");
  manifest["session_id"] = "bad-path";
  manifest["artifacts"][0]["relative_path"] = "../escape";
  check(call("POST", "/v1/sessions", manifest.dump()).status == 400,
        "path traversal rejected");
  manifest["artifacts"][0]["relative_path"] = "full.ld";
  manifest["artifacts"][0]["size"] = 2 * 1024 * 1024;
  check(call("POST", "/v1/sessions", manifest.dump()).status == 413,
        "size limits enforced");
  std::cout << "Native archive: authentication, declaration, resume, crash "
               "tail repair, commit recovery, immutability, paths and limits "
               "passed\nEvidence: "
            << root << "\n";
}
} // namespace
int main(int argc, char **argv) {
  try {
    if (argc == 3 && std::string(argv[1]) == "--self-test") {
      self_test(argv[2]);
      return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--help") {
      std::cout << "rapid-archive: native ingest; configure RAPID_ARCHIVE_* "
                   "environment. Bind behind a TLS reverse proxy.\n";
      return 0;
    }
    std::signal(SIGINT, [](int) { stopping = true; });
    std::signal(SIGTERM, [](int) { stopping = true; });
    std::signal(SIGPIPE, SIG_IGN);
    fs::path root = env("RAPID_ARCHIVE_DATA_DIR", "/data"),
             assets = env("RAPID_ASSETS_DIRECTORY", "cpp/assets");
    Archive archive(
        root,
        env("RAPID_ARCHIVE_DATABASE_PATH", (root / "archive.sqlite3").string()),
        env("RAPID_ARCHIVE_STAGING_DIR", (root / "staging").string()),
        env("RAPID_ARCHIVE_COMMITTED_DIR", (root / "committed").string()),
        env("RAPID_ARCHIVE_OWNER_USERNAME", "owner"),
        secret("RAPID_ARCHIVE_OWNER_PASSWORD_HASH",
               "/run/secrets/archive_owner_password_hash"),
        secret("RAPID_ARCHIVE_INGEST_TOKEN_HASH",
               "/run/secrets/archive_ingest_token_hash"),
        std::stoull(env("RAPID_ARCHIVE_MAX_ARTIFACT_BYTES", "17179869184")),
        assets);
    serve(env("RAPID_ARCHIVE_HOST", "127.0.0.1"),
          std::stoi(env("RAPID_ARCHIVE_PORT", "8081")),
          [&](const Request &request) { return archive.handle(request); });
    return 0;
  } catch (const std::exception &e) {
    log(std::string("ERROR archive startup: ") + e.what());
    return 1;
  }
}
