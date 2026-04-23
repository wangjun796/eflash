# Google C++ Style Guide

> **Source**: https://google.github.io/styleguide/cppguide.html
> **Last Updated**: 2025
> **Target Standard**: C++20

---

## Table of Contents

1. [Background](#background)
2. [C++ Version](#c-version)
3. [Header Files](#header-files)
4. [Scoping](#scoping)
5. [Classes](#classes)
6. [Functions](#functions)
7. [Google-Specific Magic](#google-specific-magic)
8. [Other C++ Features](#other-c-features)
9. [Inclusive Language](#inclusive-language)
10. [Naming](#naming)
11. [Comments](#comments)
12. [Formatting](#formatting)
13. [Exceptions to the Rules](#exceptions-to-the-rules)

---

## Background

C++ is one of the main development languages used by many of Google's open-source projects. As every C++ programmer knows, the language has many powerful features, but this power brings with it complexity, which in turn can make code more bug-prone and harder to read and maintain.

**The goal of this guide** is to manage this complexity by describing in detail the dos and don'ts of writing C++ code. These rules exist to keep the codebase manageable while still allowing coders to use C++ language features productively.

> **Style**, also known as readability, is what we call the conventions that govern our C++ code. The term Style is a bit of a misnomer, since these conventions cover far more than just source file formatting.

### Goals of the Style Guide

| Goal | Description |
|------|-------------|
| **Style rules should pull their weight** | The benefit of a style rule must be large enough to justify asking all engineers to remember it. |
| **Optimize for the reader, not the writer** | More time is spent reading code than writing it. Leave traces for readers at points of surprising behavior. |
| **Be consistent with existing code** | Consistency enables automation and reduces cognitive load. Pick one style and stick with it. |
| **Be consistent with the broader C++ community** | Use standard features and idioms when they solve problems effectively, but constrain them when they conflict with our needs. |
| **Avoid surprising or dangerous constructs** | High bar for waiving restrictions on features that risk program correctness. |
| **Avoid constructs that are tricky to maintain** | Complexity should be justified by widespread benefit. |
| **Be mindful of our scale** | With 100M+ lines of code, small mistakes can become costly. Avoid polluting the global namespace. |
| **Concede to optimization when necessary** | Performance optimizations can sometimes override other principles. |

---

## C++ Version

```cpp
// Currently, code should target C++20
// Do not use C++23 features yet
// Do not use non-standard extensions
```

- **Target**: C++20
- **Avoid**: C++23 features (unless explicitly approved)
- **Prohibited**: Non-standard extensions
- **Portability**: Consider portability before using C++17/C++20 features

---

## Header Files

### Self-contained Headers

```cpp
// ? Header files should be self-contained and end in .h
// ? Include header guards
// ? Include all headers you need directly

#ifndef MY_PROJECT_FOO_BAR_H_
#define MY_PROJECT_FOO_BAR_H_

#include <vector>  // Direct include, not relying on transitive includes
#include "base/logging.h"

// Inline function definitions must be in the header
inline int Add(int a, int b) { return a + b; }

#endif  // MY_PROJECT_FOO_BAR_H_
```

### The `#define` Guard

```cpp
// Format: <PROJECT>_<PATH>_<FILE>_H_
// Example: foo/src/bar/baz.h in project foo

#ifndef FOO_BAR_BAZ_H_
#define FOO_BAR_BAZ_H_

// ... content ...

#endif  // FOO_BAR_BAZ_H_
```

### Include What You Use

```cpp
// ? foo.cc should directly include bar.h if it uses symbols from bar.h
// ? Do not rely on transitive includes from foo.h

#include "bar.h"  // Direct include
#include <string> // Standard library
```

### Forward Declarations

```cpp
// ? Avoid forward declarations where possible
// ? Include the headers you need

// Bad: Hides dependencies, can break with API changes
class B;
void FuncInB();

// Good: Explicit dependency
#include "b.h"
```

**Why avoid forward declarations:**
- Can hide dependencies, causing unnecessary recompilation
- May break with compatible API changes (e.g., parameter type widening)
- Forward declaring `std::` symbols yields undefined behavior
- Can silently change code meaning when replacing `#include`

### Defining Functions in Header Files

```cpp
// ? Define functions in headers ONLY if:
// 1. Short (��10 lines), OR
// 2. Must be in header for performance/technical reasons (templates, constexpr)

class Foo {
 public:
  // OK: Short accessor
  int bar() const { return bar_; }

  // OK: Template must be in header
  template <typename T>
  void Process(T value);

  // ? Bad: Long implementation in public API
  void ComplexMethod() {
    // ... 50 lines of code ...
  }
};

// ? Better: Move long definitions to internal section or .cc file
template <typename T>
void Foo::Process(T value) {
  // Implementation here
}
```

### Names and Order of Includes

```cpp
// Order (each group separated by blank line):
// 1. Related header (the .h this .cc implements)
// 2. C system headers (<unistd.h>, <stdlib.h>)
// 3. C++ standard library headers (<string>, <vector>)
// 4. Other libraries' headers
// 5. Your project's headers

#include "foo/server/fooserver.h"  // Related header

#include <sys/types.h>             // C system
#include <unistd.h>

#include <string>                  // C++ standard
#include <vector>

#include "base/basictypes.h"       // Other libs
#include "foo/server/bar.h"
#include "third_party/absl/flags/flag.h"
```

**Rules:**
- Use quotes `"..."` for project headers, angle brackets `<...>` for system/standard headers
- Alphabetical order within each group
- No UNIX directory aliases (`.` or `..`)

---

## Scoping

### Namespaces

```cpp
// ? Place code in namespaces with unique names based on project
// ? Do not use using-directives (using namespace foo)
// ? Do not use inline namespaces

namespace my_project::my_component {  // Single-line nested namespace (preferred)

class MyClass {
 public:
  void DoSomething();
};

}  // namespace my_project::my_component
```

**Namespace Rules:**
```cpp
// ? Terminate multi-line namespaces with comments
namespace foo {

class Bar {
  // ...
};

}  // namespace foo

// ? Forbidden: using-directive pollutes namespace
using namespace foo;  // BAD

// ? Allowed: Namespace alias in .cc files or internal sections
namespace baz = ::foo::bar::baz;  // OK in .cc or internal namespace

// ? Never declare anything in namespace std
namespace std {  // UNDEFINED BEHAVIOR - NEVER DO THIS
  template <> struct hash<MyType>;
}
```

### Internal Linkage

```cpp
// ? Use unnamed namespace or static for definitions only needed in .cc file

// In foo.cc:
namespace {  // Unnamed namespace for internal linkage

void HelperFunction() {
  // Only visible in this translation unit
}

}  // namespace

// ? Or use static for functions/variables
static int internal_counter = 0;

// ? Do not use unnamed namespaces or static in .h files
```

### Nonmember, Static Member, and Global Functions

```cpp
// ? Prefer placing nonmember functions in a namespace
namespace utils {

int CalculateHash(const std::string& data);  // Namespaced nonmember

}  // namespace utils

// ? Avoid completely global functions
int GlobalFunction();  // BAD - pollutes global namespace

// ? Do not create classes just to group static members
class Utility {  // BAD - no instances, just static methods
 public:
  static void Helper1();
  static void Helper2();
};
```

### Local Variables

```cpp
// ? Declare variables in narrowest scope, initialize at declaration
void ProcessData() {
  // Good: Declaration + initialization together
  int count = GetCount();

  // Good: Loop variable declared in loop
  for (int i = 0; i < count; ++i) {
    ProcessItem(i);
  }

  // Good: Object declared close to first use
  if (needs_cache) {
    std::vector<int> cache = BuildCache();  // Declared when needed
    UseCache(cache);
  }
}

// ? Bad: Separate declaration and initialization
int x;
x = GetValue();  // BAD

// ? Bad: Variable declared far from use
std::string result;
// ... 20 lines of unrelated code ...
result = Compute();  // Hard to find declaration
```

### Static and Global Variables

```cpp
// ? Allowed: Trivially destructible types with constant initialization
constexpr int kMaxRetries = 3;
constexpr std::array<int, 3> kValidCodes = {1, 2, 3};
const char* const kErrorMessage = "Error occurred";

// ? Forbidden: Non-trivially destructible types
const std::string kConfig = LoadConfig();  // BAD - dynamic initialization
static std::map<std::string, int> g_cache; // BAD - non-trivial destructor

// ? Pattern: Function-local static pointer for complex types
Foo& GetGlobalFoo() {
  static Foo* instance = new Foo();  // OK: Never deleted, avoids destructor issues
  return *instance;
}
```

**Key Rules:**
- Objects with static storage duration must be **trivially destructible**
- Prefer `constexpr` or `constinit` for static variables
- Dynamic initialization of non-local static variables is **discouraged**
- Function-local static variables are allowed (initialized on first use)

### `thread_local` Variables

```cpp
// ? thread_local with constinit for compile-time initialization
constinit thread_local int g_thread_id = 0;

// ? Function-local thread_local (safe, initialized per-thread)
void Process() {
  thread_local std::unique_ptr<Buffer> buffer;  // OK: Function-local
  // ...
}

// ? Forbidden: thread_local with dynamic initialization at namespace scope
thread_local std::string g_data = GetConfig();  // BAD

// ? Avoid: Complex destructors that may cause use-after-free
thread_local std::vector<int> g_cache;  // Risky: Destructor order issues
```

---

## Classes

### Doing Work in Constructors

```cpp
// ? Constructors should be simple; delegate complex work
class Database {
 public:
  // Good: Simple constructor, Init() for complex setup
  explicit Database(const std::string& path) : path_(path) {}

  Status Init() {
    // Complex initialization logic here
    return Connect();
  }

 private:
  std::string path_;
};

// ? Bad: Constructor does complex work that can fail
class Database {
 public:
  Database(const std::string& path) {
    // BAD: Constructor can fail, no way to report error cleanly
    Connect();  // What if this fails?
    LoadSchema();
  }
};
```

### Implicit Conversions

```cpp
// ? Use explicit for single-argument constructors to prevent implicit conversions
class Url {
 public:
  // Good: Prevents implicit string -> Url conversion
  explicit Url(const std::string& url);
};

// ? Bad: Allows surprising implicit conversions
class Url {
 public:
  Url(const std::string& url);  // BAD: "http://..." can implicitly become Url
};

// ? Exception: Copy/move constructors, default arguments
class Widget {
 public:
  Widget(const Widget&);  // OK: Copy constructor
  Widget(Widget&&);       // OK: Move constructor
};
```

### Copyable and Movable Types

```cpp
// ? Follow the Rule of Zero/Three/Five
class Resource {
 public:
  // Good: Use smart pointers to manage ownership
  Resource(std::unique_ptr<Handle> handle) : handle_(std::move(handle)) {}

  // Delete copy operations if not copyable
  Resource(const Resource&) = delete;
  Resource& operator=(const Resource&) = delete;

  // Default move operations
  Resource(Resource&&) = default;
  Resource& operator=(Resource&&) = default;

 private:
  std::unique_ptr<Handle> handle_;
};
```

### Structs vs. Classes

```cpp
// ? Use struct for passive data carriers (all public, no invariants)
struct Point {
  int x;
  int y;
};

// ? Use class for types with invariants, methods, or private data
class Rectangle {
 public:
  Rectangle(int width, int height);
  int Area() const;

 private:
  int width_;
  int height_;
  // Invariant: width_ > 0 && height_ > 0
};
```

### Inheritance

```cpp
// ? Prefer composition over inheritance
// ? When using inheritance, make destructors virtual in base classes

class Shape {
 public:
  virtual ~Shape() = default;  // Essential for polymorphic base
  virtual void Draw() const = 0;
};

class Circle : public Shape {
 public:
  void Draw() const override;  // Use override keyword
};

// ? Avoid multiple implementation inheritance (except for interfaces)
// ? Never use private inheritance unless implementing "is-implemented-in-terms-of"
```

### Operator Overloading

```cpp
// ? Overload operators only when semantics are obvious and intuitive
class Money {
 public:
  Money operator+(const Money& other) const;  // Clear: monetary addition
  bool operator==(const Money& other) const;  // Clear: value comparison
};

// ? Avoid overloading operators with non-obvious semantics
class NetworkPacket {
 public:
  // BAD: What does packet1 + packet2 mean? Concatenate? Merge?
  NetworkPacket operator+(const NetworkPacket& other) const;
};
```

### Access Control

```cpp
// ? Order: public, protected, private
// ? Each section indented 1 space, members indented 2 spaces

class MyClass {
 public:      // 1 space indent
  MyClass();  // 2 space indent
  void PublicMethod();

 protected:   // Blank line before (optional in small classes)
  void ProtectedHelper();

 private:
  int private_data_;
  void PrivateMethod();
};
```

### Declaration Order

```cpp
// ? Within each access section, order declarations:
// 1. Typedefs and enums
// 2. Constants (static constexpr)
// 3. Constructors
// 4. Destructor
// 5. Methods (in order of likely use)
// 6. Static methods
// 7. Private data members

class Widget {
 public:
  using Id = int64_t;

  static constexpr int kDefaultSize = 100;

  explicit Widget(Id id);
  ~Widget();

  void Draw() const;
  void Resize(int w, int h);

  static Widget CreateDefault();

 private:
  Id id_;
  int width_;
  int height_;
};
```

---

## Functions

### Inputs and Outputs

```cpp
// ? Use const references for read-only input parameters
void ProcessData(const std::string& input, const Config& config);

// ? Use pointers for optional parameters or output parameters
bool ParseInput(const std::string& src, std::string* output);  // output is optional

// ? Use std::optional<T> for truly optional values (C++17)
std::optional<User> FindUserById(int64_t id);

// ? Return complex outputs by value (rely on move semantics)
std::vector<Result> ComputeResults(const Input& input);

// ? Avoid non-const reference parameters (confusing: input or output?)
void Process(std::string& data);  // BAD: Is data modified?
```

### Write Short Functions

```cpp
// ? Functions should generally be short and focused
// ? If a function exceeds ~40 lines, consider extracting helpers

// Good: Focused, single responsibility
Status ValidateUser(const User& user) {
  if (!IsValidEmail(user.email())) return Status::INVALID_EMAIL;
  if (!IsValidName(user.name())) return Status::INVALID_NAME;
  if (user.age() < kMinAge) return Status::UNDERAGE;
  return Status::OK;
}

// Better: Extract complex validation logic
static bool IsValidEmail(absl::string_view email) {
  // ... email validation logic ...
}
```

### Function Overloading

```cpp
// ? Use overloading when it improves clarity and the behavior is consistent
class Printer {
 public:
  void Print(int value);
  void Print(double value);
  void Print(absl::string_view text);
};

// ? Avoid overloading when behavior differs significantly
class Parser {
 public:
  // BAD: Same name, completely different behaviors
  Result Parse(const std::string& json);      // Parses JSON
  Result Parse(int flags);                    // Configures parser
};
```

### Default Arguments

```cpp
// ? Use default arguments for optional parameters at the end of parameter list
void Connect(const std::string& host,
             int port = 8080,
             absl::Duration timeout = absl::Seconds(30));

// ? Avoid default arguments that hide important behavior
// ? Never use default arguments for output parameters
void Process(Input in, Output* out = nullptr);  // BAD: Unclear if out is optional
```

### Trailing Return Type Syntax

```cpp
// ? Use trailing return types when the return type is complex or depends on template parameters
template <typename T, typename U>
auto Add(T a, U b) -> decltype(a + b);

// ? Use for readability when return type is long
auto GetVeryLongTypeName()
    -> std::map<std::string, std::vector<std::unique_ptr<ComplexType>>>;

// ? Avoid for simple return types
int GetCount();  // Better than: auto GetCount() -> int;
```

---

## Google-Specific Magic

### Ownership and Smart Pointers

```cpp
// ? Use std::unique_ptr for exclusive ownership
std::unique_ptr<Database> db = std::make_unique<Database>(config);

// ? Use std::shared_ptr when ownership is genuinely shared
std::shared_ptr<ConnectionPool> pool = std::make_shared<ConnectionPool>();

// ? Use raw pointers for non-owning observation (function parameters, return values)
void Process(const Database* db);  // db is observed, not owned
Database* GetActiveDatabase();     // Caller does not take ownership

// ? Never use std::auto_ptr (deprecated)
// ? Avoid std::shared_ptr for performance-critical code unless necessary
```

### cpplint

```bash
# Use cpplint to check style compliance
# Install: pip install cpplint

# Run on a file
cpplint myfile.cc

# Run on a directory
cpplint --recursive src/

# Common flags
cpplint --filter=-whitespace/line_length  # Disable specific check
cpplint --linelength=100                   # Custom line length
```

---

## Other C++ Features

### Rvalue References

```cpp
// ? Use std::move() to explicitly cast to rvalue when transferring ownership
std::vector<int> CreateVector() {
  std::vector<int> v;
  // ... populate v ...
  return v;  // NRVO or move, no need for std::move on return
}

void Process() {
  std::vector<int> data = CreateVector();
  Consume(std::move(data));  // Explicitly transfer ownership
  // data is now in valid but unspecified state
}

// ? Avoid std::move() on local variables being returned (can inhibit NRVO)
std::vector<int> Bad() {
  std::vector<int> v;
  return std::move(v);  // BAD: May prevent copy elision
}
```

### Friends

```cpp
// ? Use friend sparingly, only when necessary for encapsulation
class Buffer {
 public:
  // Good: Friend allows controlled access to internals
  friend class BufferPool;  // BufferPool manages Buffer lifecycle

 private:
  void* internal_ptr_;  // Hidden from general API
};

// ? Avoid friend for convenience or to bypass access control
```

### Exceptions

```cpp
// ? Do not use C++ exceptions in Google C++ code
// ? Use Status or absl::Status for error handling

absl::StatusOr<User> FindUser(int64_t id) {
  if (id < 0) {
    return absl::InvalidArgumentError("ID must be non-negative");
  }
  // ... lookup logic ...
  return user;
}

// Usage:
auto result = FindUser(123);
if (!result.ok()) {
  LOG(ERROR) << "Failed: " << result.status();
  return;
}
User user = std::move(*result);
```

### `noexcept`

```cpp
// ? Use noexcept for functions that guarantee not to throw
// ? Especially important for move operations and swap

class Widget {
 public:
  Widget(Widget&& other) noexcept;  // Move constructor should be noexcept
  void swap(Widget& other) noexcept;  // Enable efficient container operations
};

// ? Do not use noexcept unless you can guarantee no exceptions will escape
```

### Casting

```cpp
// ? Use C++-style casts, not C-style casts
static_cast<int>(value);      // Numeric conversion
dynamic_cast<Derived*>(base); // Safe downcast (with RTTI)
const_cast<T*>(ptr);          // Remove const (use sparingly)
reinterpret_cast<uintptr_t>(ptr);  // Low-level bit pattern conversion

// ? Avoid C-style casts: (int)value - ambiguous and dangerous

// ? Prefer type-safe alternatives when possible
absl::implicit_cast<T>(value);  // Documents intent for implicit conversion
```

### Use of `const`

```cpp
// ? Use const for:
// - Function parameters that are not modified
// - Member functions that do not modify state
// - Local variables that are not reassigned

void Process(const std::string& input);  // input not modified

class Cache {
 public:
  int GetSize() const;  // Does not modify Cache state
 private:
  mutable std::atomic<int> hit_count_;  // mutable for thread-safe counters
};

void Compute() {
  const int limit = CalculateLimit();  // limit never changes
  // ...
}
```

### Use of `constexpr`, `constinit`, `consteval`

```cpp
// ? Use constexpr for compile-time computation
constexpr int Factorial(int n) {
  return (n <= 1) ? 1 : n * Factorial(n - 1);
}
constexpr int k5Fact = Factorial(5);  // Computed at compile time

// ? Use constinit for static variables requiring static initialization
constinit static std::array<int, 100> kLookup = BuildLookupTable();

// ? Use consteval for functions that must be evaluated at compile time (C++20)
consteval int CompileTimeHash(const char* str);

// ? Do not use constexpr for functions with side effects or runtime dependencies
```

### Integer Types

```cpp
// ? Use fixed-width types from <stdint.h> when size matters
#include <stdint.h>
int32_t user_id;      // Exactly 32 bits
uint64_t file_size;   // Exactly 64 bits, unsigned

// ? Use size_t for sizes, ptrdiff_t for pointer differences
std::vector<char> buffer;
size_t len = buffer.size();

// ? Use int for general-purpose integers when size doesn't matter
int count = 0;
for (int i = 0; i < count; ++i) { ... }

// ? Avoid plain char for numeric values (signedness is implementation-defined)
// ? Avoid long/long long unless interfacing with APIs that require them
```

### Floating-Point Types

```cpp
// ? Use double for most floating-point calculations
double temperature = 98.6;
double CalculateArea(double radius);

// ? Use float only when memory/bandwidth is critical (e.g., large arrays, GPU)
std::vector<float> vertex_buffer;  // OK: Graphics data

// ? Use long double only when interfacing with APIs that require it

// ? Avoid comparing floats for exact equality
if (a == b) { ... }  // BAD: Floating-point precision issues

// ? Use approximate comparison when needed
if (std::abs(a - b) < 1e-9) { ... }
```

### Preprocessor Macros

```cpp
// ? Use macros only for:
// - Header guards
// - Conditional compilation (#ifdef for platform-specific code)
// - Simple, well-documented constants (prefer constexpr when possible)

#ifndef MY_HEADER_H_
#define MY_HEADER_H_
// ...
#endif

#ifdef _WIN32
#include <windows.h>
#endif

// ? If you must use function-like macros:
// - Use parentheses around parameters and the entire expression
// - Use do { ... } while(0) for multi-statement macros
#define MAX(a, b) ((a) > (b) ? (a) : (b))

#define LOG_IF(cond, msg) \
  do { \
    if (cond) { \
      std::cerr << msg << std::endl; \
    } \
  } while (0)

// ? Avoid macros for:
// - Code generation (use templates or constexpr instead)
// - Complex logic (use inline functions)
// - Constants (use constexpr or enum)
```

### 0 and `nullptr`/`NULL`

```cpp
// ? Use nullptr for null pointer constants (C++11 and later)
int* ptr = nullptr;
if (ptr == nullptr) { ... }

// ? Avoid NULL (macro for 0 or 0L, can cause overload resolution issues)
int* ptr = NULL;  // BAD

// ? Avoid plain 0 for pointers (less clear intent)
int* ptr = 0;  // Acceptable but nullptr is preferred

// ? Exception: 0 is fine for integer contexts
int count = 0;
if (count == 0) { ... }
```

### `sizeof`

```cpp
// ? Use sizeof(var) instead of sizeof(type) when possible
int arr[10];
size_t size = sizeof(arr);  // Clear: size of the variable

// ? Use sizeof(*ptr) for pointer-to-type size
int* ptr;
size_t element_size = sizeof(*ptr);  // Size of int

// ? Avoid sizeof(Type) when the variable is available (less maintainable)
```

### Type Deduction (including `auto`)

```cpp
// ? Use auto when:
// - The type is obvious from context or initialization
// - The type name is long/complex (iterators, lambdas, templates)
// - To avoid type duplication

auto it = map.find(key);  // Clear: iterator type
auto lambda = [](int x) { return x * 2; };  // Unique closure type

// ? Use auto with trailing return types for generic code
template <typename T, typename U>
auto Add(T a, U b) -> decltype(a + b);

// ? Avoid auto when:
// - The type is not obvious and affects understanding of behavior
// - You need a specific type conversion (use explicit cast instead)

auto result = Compute();  // BAD: What type is result? StatusOr<T>? T? Status?

// ? Prefer structured bindings for tuple-like returns (C++17)
auto [name, age, email] = GetUser(id);
```

### Lambda Expressions

```cpp
// ? Use lambdas for short, localized callbacks
std::sort(vec.begin(), vec.end(),
          [](const Item& a, const Item& b) { return a.priority > b.priority; });

// ? Use [=] or [&] capture lists explicitly
int multiplier = 2;
auto scale = [multiplier](int x) { return x * multiplier; };  // Capture by value

// ? For mutable lambdas or complex captures, document intent
auto counter = [count = 0]() mutable { return count++; };  // Capture with initializer

// ? Avoid complex lambdas that span multiple lines with many captures
// ? Extract to named function if lambda becomes hard to read

// ? Format: Parameters and body like regular functions, capture list like comma-separated list
auto complex = [this, &data, value = ComputeDefault()](int x, int y) -> Result {
  // Multi-line lambda body
  return Process(data, x, y, value);
};
```

### Switch Statements

```cpp
// ? Always use braces for case blocks (optional but recommended for consistency)
switch (status) {
  case Status::OK: {
    HandleSuccess();
    break;
  }
  case Status::ERROR: {
    HandleError();
    break;
  }
  default: {
    LOG(FATAL) << "Unknown status: " << status;
  }
}

// ? Use [[fallthrough]] attribute for intentional fall-through (C++17)
switch (level) {
  case 3:
    PrepareLevel3();
    [[fallthrough]];
  case 2:
    PrepareLevel2();
    [[fallthrough]];
  case 1:
    PrepareLevel1();
    break;
}

// ? Avoid fall-through without comment or attribute
switch (x) {
  case 1:
    DoA();
  case 2:  // BAD: Did we forget break here?
    DoB();
    break;
}
```

---

## Inclusive Language

```cpp
// ? Use inclusive, respectful terminology
// ? Avoid terms with problematic historical connotations

// Preferred alternatives:
// - "allowlist/denylist" instead of "whitelist/blacklist"
// - "primary/secondary" or "main/replica" instead of "master/slave"
// - "iterator" instead of "pointer" when discussing generic iteration

// ? When interfacing with external APIs that use non-inclusive terms:
// - Wrap or alias the API to use inclusive names internally
// - Document the mapping clearly
```

---

## Naming

### Choosing Names

```cpp
// ? Names should be:
// - Descriptive and unambiguous
// - Consistent with existing code in the project
// - Following the conventions below

// ? Avoid:
// - Abbreviations unless widely known (OK: HTTP, URL; BAD: cnt, tmp)
// - Names that differ only by case or underscores
// - Names that are keywords or too similar to keywords
```

### File Names

```cpp
// ? Lowercase with underscores: my_file.cc, my_file.h
// ? Match file name to primary class/type when possible: url_parser.cc -> class UrlParser

// Examples:
// - http_server.cc / http_server.h
// - string_util_test.cc  // Test files end with _test.cc
```

### Type Names

```cpp
// ? Types (classes, structs, enums, type aliases): CamelCase, starting with capital
class UrlTable;
class UrlTableTester;
struct UrlTableProperties;
enum class OpenFlags { kRead, kWrite };
using UrlTablePropertiesMap = std::map<std::string, UrlTableProperties>;

// ? Template parameters: CamelCase, often single letter for simple cases
template <typename T>
class Array;
template <typename Key, typename Value>
class Map;
```

### Variable Names

```cpp
// ? Variables (including function parameters, data members): lowercase with underscores
std::string table_name;
int num_errors;
bool is_valid;

// ? Class data members: trailing underscore
class MyClass {
 private:
  int size_;
  std::string name_;
};

// ? Loop variables: short names acceptable when scope is tiny
for (int i = 0; i < n; ++i) { ... }
for (const auto& item : items) { ... }
```

### Constant Names

```cpp
// ? Constants (compile-time, namespace/class scope): kConstantName with CamelCase after k
constexpr int kMaxRetries = 3;
constexpr absl::string_view kDefaultEndpoint = "https://api.example.com";

// ? Enum values: kEnumValue or just Value depending on enum class style
enum class Color { kRed, kGreen, kBlue };
// or
enum class Status { OK, ERROR, TIMEOUT };  // If enum class name provides context
```

### Function Names

```cpp
// ? Functions: lowercase with underscores
void ProcessHttpRequest();
std::string GetUserName();
bool IsValidEmail(absl::string_view email);

// ? Accessors/mutators: Get/Set/Is/Has prefix when appropriate
class User {
 public:
  const std::string& name() const;  // Preferred: no Get prefix for simple accessors
  void set_name(const std::string& name);
  bool is_active() const;
  bool has_permission(Permission p) const;
};
```

### Namespace Names

```cpp
// ? Namespaces: lowercase with underscores, based on project/path
namespace my_project::network::http;
namespace utils::string;

// ? Internal namespaces: include "internal" in name
namespace my_project::internal::details;
```

### Macro Names

```cpp
// ? Macros: ALL_CAPS with underscores
#define MY_PROJECT_MAX_BUFFER_SIZE 4096
#define CHECK_NOT_NULL(ptr) CHECK((ptr) != nullptr)

// ? Function-like macros: Use parentheses around parameters
#define MIN(a, b) ((a) < (b) ? (a) : (b))
```

### Exceptions to Naming Rules

```cpp
// ? When interfacing with external libraries/APIs:
// - Follow the external naming convention for compatibility
// - Wrap external types with Google-style wrappers when possible

// ? When matching standard library conventions:
// - Use std::string::size_type, not size_type (qualified)
// - Follow standard algorithm naming (std::sort, std::find)
```

---

## Comments

### Comment Style

```cpp
// ? Use // for both regular and multi-line comments
// Start with a capital letter, end with punctuation
// One space between // and comment text

// Good:
// Returns the number of users in the system.
// If the cache is stale, refreshes from the database.

// Bad:
// returns number of users
// if cache stale, refresh  <-- No capitalization/punctuation
```

### File Comments

```cpp
// ? Top of each file: copyright, author, brief description
// Copyright 2024 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// ...
//
// File: url_parser.cc
// Author: john.doe@example.com
//
// Parses URLs into components (scheme, host, path, query, fragment).
// Thread-safe for read-only operations.

// ? Include high-level overview of file's purpose and usage
```

### Function Comments

```cpp
// ? Every function declaration should have a comment describing:
// - What it does (not how - that's implementation)
// - Parameters and return value
// - Pre-conditions and post-conditions
// - Thread-safety properties
// - Ownership transfer (if any)

// Example:
// Parses a URL string and returns its components.
//
// Args:
//   url: The URL to parse. Must be a valid absolute URL with scheme.
//   allow_relative: If true, relative URLs are accepted (scheme defaults to "http").
//
// Returns:
//   ParsedUrl on success, or absl::InvalidArgumentError if parsing fails.
//
// Thread-safety: Safe for concurrent calls with different inputs.
// Ownership: Caller retains ownership of input string.
absl::StatusOr<ParsedUrl> ParseUrl(absl::string_view url, bool allow_relative);
```

### Variable Comments

```cpp
// ? Class data members: Comment if purpose isn't clear from name/type
class Request {
 private:
  // Timestamp when the request was first received (Unix epoch microseconds).
  int64_t received_time_us_;

  // Bitmask of RequestFlags indicating processing options.
  uint32_t flags_;

  // No comment needed: name and type are self-explanatory
  std::string user_agent_;
};

// ? Global variables: Always comment purpose and why global
// Total number of requests processed since server start.
// Stored globally for metrics collection; updated atomically.
std::atomic<int64_t> g_total_requests{0};
```

### Implementation Comments

```cpp
// ? Use comments to explain WHY, not WHAT (code shows what)
// ? Comment tricky, non-obvious, or important implementation details

// Good:
// Use binary search because the list is sorted and large (>10k items).
// Linear search would be O(n) vs O(log n) here.
auto it = std::lower_bound(sorted_ids.begin(), sorted_ids.end(), target);

// Good:
// This lock must be acquired before checking the cache to prevent
// race conditions with the background refresh thread.
absl::MutexLock lock(&cache_mutex_);
if (!cache.Has(key)) {
  RefreshCache(key);
}

// Bad:
// Increment i by 1  <-- Obvious from code
i++;
```

### TODO Comments

```cpp
// ? Format: TODO(username or bug): Description with context
// ? Include bug ID, date, or event for time-bound TODOs

// Styles (in order of preference):
// TODO: bug 12345678 - Remove after Q4 2024 compatibility window.
// TODO: example.com/design-doc - Refactor after new API is stable.
// TODO(john.doe): Use absl::string_view instead of const std::string&.
// TODO(bug 98765): Handle edge case when input is empty.

// ? For time-bound TODOs, be specific:
// TODO: Remove this workaround after all clients migrate to v2 API (by 2024-12-31).
```

---

## Formatting

### Line Length

```cpp
// ? Maximum line length: 80 characters
// ? Exceptions (may exceed 80 chars):
// - Comments with long URLs or commands
// - String literals that are hard to split (URLs, regex, embedded languages)
// - Include statements
// - Header guards

// Example of acceptable long line:
// See https://example.com/very/long/documentation/path/for/detailed/specifications

// Example of splitting long string:
const char* kHelpText =
    "This is a long help message that should be split across multiple "
    "lines for readability while staying within the 80-character limit.";
```

### Spaces vs. Tabs

```cpp
// ? Use spaces only, 2 spaces per indentation level
// ? Configure editor to insert spaces when Tab is pressed

// Good:
void Function() {
  if (condition) {  // 2 spaces
    DoSomething();  // 4 spaces
  }
}

// ? Never use tab characters for indentation
```

### Function Declarations and Definitions

```cpp
// ? Return type on same line as function name
// ? Parameters on same line if they fit; wrap at parenthesis if not
// ? Open brace on same line as function declaration

// Formats:
ReturnType FunctionName(Type param1, Type param2) {
  // Body
}

ReturnType LongClassName::ReallyLongFunctionName(Type param1, Type param2,
                                                  Type param3) {
  // Body
}

ReturnType ClassName::FunctionThatDoesntFitOnOneLine(
    Type param1,  // 4-space indent for wrapped params
    Type param2,
    Type param3) {
  // Body with 2-space indent
}
```

### Braced Initializer Lists

```cpp
// ? Format braced lists like function calls
return {foo, bar};

SomeType variable{
    some_value,
    another_value,
    {nested, list},
};

// ? When braced list follows a name, treat {} like function parentheses
std::vector<int> v = {1, 2, 3};
MyType m{arg1, arg2, {nested}};
```

### Pointer and Reference Expressions

```cpp
// ? No spaces around . or ->
obj.method();
ptr->field;

// ? Pointer/reference operators stick to type, not variable
char* ptr;           // Not: char * ptr
const std::string& s; // Not: const std::string & s

// ? Space between type and variable name
int* pointer;  // Type is "int*", variable is "pointer"

// ? Never declare multiple pointer/reference variables in one statement
int *p1, *p2;  // BAD: Misleading, looks like p2 is int, not int*
```

### Boolean Expressions

```cpp
// ? When wrapping boolean expressions, be consistent with operator placement
// ? Prefer operators at end of line (Google style)

if (this_condition &&
    that_condition &&
    another_condition) {
  // Body
}

// ? Use parentheses to clarify complex expressions
if ((a && b) || (c && d)) { ... }

// ? Use punctuation operators, not word operators
if (x && y) { ... }  // Good
if (x and y) { ... } // Avoid
```

### Horizontal Whitespace

```cpp
// ? General spacing rules:
x = 5;              // Space around assignment
if (condition) {    // Space after keyword, before brace
  a = b + c;        // Space around binary operators
  f(x, y, z);       // No space after (, before ), space after commas
}

// ? Inside parentheses: usually no spaces
if (IsValid(x)) { ... }
// Rare exception for readability:
if ( (a && b) || c ) { ... }  // OK if it clarifies grouping

// ? Template and cast syntax:
std::vector<int> v;     // No space inside < >
static_cast<int>(x);    // No space between type and (
```

### Vertical Whitespace

```cpp
// ? Use blank lines sparingly to group related code
// ? Avoid blank lines at start/end of blocks

class Widget {
 public:
  Widget();
  ~Widget();

  void Draw() const;
  void Resize(int w, int h);

 private:
  int width_;
  int height_;
  bool visible_;
};

// ? One blank line between function definitions
void Widget::Draw() const {
  // Implementation
}

void Widget::Resize(int w, int h) {
  // Implementation
}
```

---

## Exceptions to the Rules

### Existing Non-conformant Code

```cpp
// ? When modifying legacy code that doesn't follow this guide:
// - Maintain consistency with surrounding code
// - Clean up style in a separate, focused change if needed
// - Do not mix style fixes with functional changes

// Example: If surrounding code uses 4-space indent, use 4-space in your change
// (but consider proposing a project-wide style update separately)
```

### Windows Code

```cpp
// ? When writing Windows-specific code:
// - Still follow Google naming (lowercase_with_underscores, .cc extension)
// - Use Windows types (DWORD, HANDLE) when calling Windows APIs
// - Prefer underlying C++ types when possible (const TCHAR* vs LPCTSTR)

// ? Compiler settings:
// - Warning level 3 or higher
// - Treat warnings as errors

// ? Include guards: Use standard Google style, not #pragma once

// ? Exceptions allowed:
// - Multiple inheritance for COM/ATL interfaces
// - STL exceptions when required by platform STL (but don't write your own exception handling)

// Example:
#ifdef _WIN32
#include <windows.h>
#include <tchar.h>

void WindowsSpecificFunction() {
  HANDLE handle = ::CreateFile(...);  // Using Windows types is OK
  // ...
}
#endif  // _WIN32
```

---

## Quick Reference: `.clang-format` for Google Style

```yaml
# .clang-format
BasedOnStyle: Google
Language: Cpp
Cpp11BracedListStyle: true
PointerAlignment: Left
AccessModifierOffset: -2
AlignAfterOpenBracket: Align
AlignConsecutiveAssignments: false
AlignConsecutiveDeclarations: false
AlignEscapedNewlinesLeft: true
AlignOperands: true
AlignTrailingComments: true
AllowAllParametersOfDeclarationOnNextLine: true
AllowShortBlocksOnASingleLine: false
AllowShortCaseLabelsOnASingleLine: false
AllowShortFunctionsOnASingleLine: Inline
AllowShortIfStatementsOnASingleLine: false
AllowShortLoopsOnASingleLine: false
AlwaysBreakAfterDefinitionReturnType: None
AlwaysBreakAfterReturnType: None
AlwaysBreakBeforeMultilineStrings: true
AlwaysBreakTemplateDeclarations: true
BinPackArguments: true
BinPackParameters: true
BraceWrapping:
  AfterClass: false
  AfterControlStatement: false
  AfterEnum: false
  AfterFunction: false
  AfterNamespace: false
  AfterObjCDeclaration: false
  AfterStruct: false
  AfterUnion: false
  BeforeCatch: false
  BeforeElse: false
  IndentBraces: false
BreakBeforeBinaryOperators: None
BreakBeforeBraces: Attach
BreakBeforeTernaryOperators: true
BreakConstructorInitializersBeforeComma: false
ColumnLimit: 80
CommentPragmas: '^ IWYU pragma:'
ConstructorInitializerAllOnOneLineOrOnePerLine: true
ConstructorInitializerIndentWidth: 4
ContinuationIndentWidth: 4
ExperimentalAutoDetectBinPacking: false
ForEachMacros: [ foreach, Q_FOREACH, BOOST_FOREACH ]
IncludeCategories:
  - Regex: '^<.*'
    Priority: 2
  - Regex: '^".*'
    Priority: 1
IndentCaseLabels: true
IndentWidth: 2
IndentWrappedFunctionNames: false
JavaScriptQuotes: Leave
JavaScriptWrapImports: true
KeepEmptyLinesAtTheStartOfBlocks: false
MacroBlockBegin: ''
MacroBlockEnd: ''
MaxEmptyLinesToKeep: 1
NamespaceIndentation: None
ObjCBlockIndentWidth: 2
ObjCSpaceAfterProperty: false
ObjCSpaceBeforeProtocolList: true
PenaltyBreakBeforeFirstCallParameter: 1
PenaltyBreakComment: 300
PenaltyBreakFirstLessLess: 120
PenaltyBreakString: 1000
PenaltyExcessCharacter: 1000000
PenaltyReturnTypeOnItsOwnLine: 200
ReflowComments: true
SortIncludes: true
SpaceAfterCStyleCast: false
SpaceAfterTemplateKeyword: true
SpaceBeforeAssignmentOperators: true
SpaceBeforeParens: ControlStatements
SpaceInEmptyParentheses: false
SpacesBeforeTrailingComments: 2
SpacesInAngles: false
SpacesInContainerLiterals: true
SpacesInCStyleCastParentheses: false
SpacesInParentheses: false
SpacesInSquareBrackets: false
Standard: c++20
TabWidth: 2
UseTab: Never
```

**Usage:**
```bash
# Format a file
clang-format -i -style=file myfile.cc

# Check formatting without modifying
clang-format --dry-run --Werror -style=file **/*.cc **/*.h

# Format with Google style directly (no config file)
clang-format -i -style=Google myfile.cc
```

---

> **Final Note**: This guide provides maximal guidance with reasonable restrictions. Common sense and good taste should prevail. When in doubt, ask your project leads. The absence of a prohibition is not a license to proceed with clever or unusual constructs.