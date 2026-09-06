#include "rapid/native.hpp"
#include <curl/curl.h>
#include <fstream>
#include <thread>

namespace rapid::native {
namespace {
struct Reply {
  std::string body;
  std::uint64_t offset = 0;
};
Reply request(const Config &c, const std::string &method,
              const std::string &url, const std::string &body = "",
              std::uint64_t offset = 0) {
  // Only send credentials back to the configured archive origin/path prefix.
  if (!url.starts_with(c.upload_url + "/") ||
      url.find('\r') != std::string::npos ||
      url.find('\n') != std::string::npos)
    throw std::runtime_error("archive returned an unrelated upload URL");
  auto *raw = curl_easy_init();
  if (!raw)
    throw std::runtime_error("curl init failed");
  std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(raw,
                                                           curl_easy_cleanup);
  Reply response;
  curl_slist *headers = nullptr;
  headers = curl_slist_append(
      headers, ("Authorization: Bearer " + c.upload_token).c_str());
  headers = curl_slist_append(
      headers, method == "PATCH"
                   ? "Content-Type: application/offset+octet-stream"
                   : "Content-Type: application/json");
  headers = curl_slist_append(
      headers, ("Upload-Offset: " + std::to_string(offset)).c_str());
  std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> owned(
      headers, curl_slist_free_all);
  curl_easy_setopt(raw, CURLOPT_URL, url.c_str());
  curl_easy_setopt(raw, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(raw, CURLOPT_CUSTOMREQUEST, method.c_str());
  curl_easy_setopt(raw, CURLOPT_CONNECTTIMEOUT, 5L);
  curl_easy_setopt(raw, CURLOPT_TIMEOUT, 30L);
  curl_easy_setopt(raw, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(raw, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(raw, CURLOPT_PROTOCOLS_STR, "http,https");
  if (method == "HEAD")
    curl_easy_setopt(raw, CURLOPT_NOBODY, 1L);
  else {
    curl_easy_setopt(raw, CURLOPT_POSTFIELDS, body.data());
    curl_easy_setopt(raw, CURLOPT_POSTFIELDSIZE_LARGE, curl_off_t(body.size()));
  }
  curl_easy_setopt(
      raw, CURLOPT_WRITEFUNCTION,
      +[](char *p, std::size_t a, std::size_t b, void *user) -> std::size_t {
        auto &r = *static_cast<Reply *>(user);
        auto n = a * b;
        if (r.body.size() + n > 2 * 1024 * 1024)
          return 0;
        r.body.append(p, n);
        return n;
      });
  curl_easy_setopt(raw, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(
      raw, CURLOPT_HEADERFUNCTION,
      +[](char *p, std::size_t a, std::size_t b, void *user) -> std::size_t {
        std::string h(p, a * b);
        for (auto &ch : h)
          ch = std::tolower(static_cast<unsigned char>(ch));
        if (h.starts_with("upload-offset:"))
          try {
            static_cast<Reply *>(user)->offset = std::stoull(h.substr(14));
          } catch (...) {
            return 0;
          }
        return a * b;
      });
  curl_easy_setopt(raw, CURLOPT_HEADERDATA, &response);
  curl_easy_setopt(raw, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(
      raw, CURLOPT_XFERINFOFUNCTION,
      +[](void *, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
        return stopping ? 1 : 0;
      });
  auto result = curl_easy_perform(raw);
  long status = 0;
  curl_easy_getinfo(raw, CURLINFO_RESPONSE_CODE, &status);
  if (result != CURLE_OK || status < 200 || status >= 300)
    throw std::runtime_error("archive request failed (HTTP " +
                             std::to_string(status) + ")");
  return response;
}
void upload(const Config &c, const fs::path &directory) {
  auto manifest = Json::parse(read_file(directory / "manifest.json"));
  auto declared = Json::parse(
      request(c, "POST", c.upload_url + "/v1/sessions", manifest.dump()).body);
  auto resolve = [&](const std::string &url) {
    return url.starts_with("/v1/") ? c.upload_url + url : url;
  };
  if (declared.at("artifacts").size() != manifest.at("artifacts").size())
    throw std::runtime_error("archive declaration count mismatch");
  for (std::size_t i = 0; i < manifest["artifacts"].size(); ++i) {
    const auto &item = manifest["artifacts"][i];
    auto relative = fs::path(item.at("relative_path").get<std::string>());
    if (relative.is_absolute())
      throw std::runtime_error("invalid artifact path");
    for (const auto &part : relative)
      if (part == "..")
        throw std::runtime_error("invalid artifact path");
    auto source = directory / relative;
    std::uint64_t size = item.at("size");
    if (fs::file_size(source) != size ||
        hash_file(source) != string(item, "sha256"))
      throw std::runtime_error("local artifact integrity mismatch");
    auto url = resolve(declared["artifacts"][i].at("url").get<std::string>());
    auto offset = request(c, "HEAD", url).offset;
    if (offset > size)
      throw std::runtime_error("archive offset exceeds artifact");
    std::ifstream file(source, std::ios::binary);
    file.seekg(offset);
    while (offset < size && !stopping) {
      std::string chunk(std::min<std::uint64_t>(size - offset, 1024 * 1024),
                        '\0');
      file.read(chunk.data(), chunk.size());
      if (std::size_t(file.gcount()) != chunk.size())
        throw std::runtime_error("artifact read failed");
      auto next = request(c, "PATCH", url, chunk, offset).offset;
      if (next != offset + chunk.size())
        throw std::runtime_error("archive did not acknowledge exact chunk");
      offset = next;
    }
  }
  if (!stopping)
    request(c, "POST", resolve(declared.at("commit_url").get<std::string>()));
}
} // namespace
void upload_loop(Runtime &runtime, const Config &config) {
  curl_global_init(CURL_GLOBAL_DEFAULT);
  Database db(config.queue);
  db.exec("CREATE TABLE IF NOT EXISTS jobs(path TEXT PRIMARY KEY,state TEXT "
          "NOT NULL,error TEXT,updated_at REAL NOT NULL)");
  while (!stopping) {
    auto jobs = db.query("SELECT path FROM jobs WHERE state!='complete' ORDER "
                         "BY updated_at LIMIT 1");
    if (!jobs.empty()) {
      auto path = jobs[0]["path"].get<std::string>();
      try {
        runtime.upload_state("uploading");
        upload(config, path);
        if (!stopping) {
          db.exec("UPDATE jobs SET state='complete',error=NULL,updated_at=? "
                  "WHERE path=?",
                  {double(std::time(nullptr)), path});
          runtime.upload_state("complete");
          continue;
        }
      } catch (const std::exception &e) {
        db.exec(
            "UPDATE jobs SET state='retry',error=?,updated_at=? WHERE path=?",
            {std::string(e.what()).substr(0, 500), double(std::time(nullptr)),
             path});
        runtime.upload_state("retrying");
        log("WARNING archive upload deferred");
      }
    }
    for (int i = 0; i < (jobs.empty() ? 10 : 300) && !stopping; ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  curl_global_cleanup();
}
} // namespace rapid::native
