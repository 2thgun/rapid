// The protocol parser is intentionally self-contained until the deployment selects
// a vetted JSON package (simdjson or Boost.JSON).  This translation unit reserves
// the JSON boundary for that dependency and keeps the core library layout stable.
namespace rapid {}
