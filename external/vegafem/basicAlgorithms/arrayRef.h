#ifndef ARRAYREF_H
#define ARRAYREF_H

#include <vector>
#include <array>

// a template class to refer to an external array of basic values
template<typename T>
class ArrayRef
{
public:

  typedef T value_type;

  ArrayRef() {} // empty array
  ArrayRef(int numElements, const T * elements) : n(numElements), v(elements) {}
  template<class Allocator>
  ArrayRef(const std::vector<T, Allocator> & vec) : n(static_cast<int>(vec.size())), v(vec.data()) {}
  template<std::size_t N>
  ArrayRef(const std::array<T, N> & arr) : n(arr.size()), v(arr.data()) {}

  int size() const { return n; }
  const T * data() const { return v; }
  const T * begin() const { return v; }
  const T * end() const { return v + n; }

  const T & operator[](int ID) const { return v[ID]; }

protected:
  int n = 0;
  const T * v = nullptr;
};

template<typename T>
ArrayRef<T> makeArrayRef(int numElements, const T * elements) { return ArrayRef<T>(numElements, elements); }

template<typename T, class Allocator>
ArrayRef<T> makeArrayRef(const std::vector<T, Allocator> & vec) { return ArrayRef<T>(vec); }

template<typename T, std::size_t N>
ArrayRef<T> makeArrayRef(const std::array<T, N> & arr) { return ArrayRef<T>(arr); }

#endif
