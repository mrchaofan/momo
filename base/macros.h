#pragma once

// 通用宏。header-only, 属于 momo_base(engine/platform 亦可使用)。

#define MOMO_DISALLOW_COPY_AND_ASSIGN(TypeName) \
  TypeName(const TypeName&) = delete;           \
  TypeName& operator=(const TypeName&) = delete

#define MOMO_CONCAT_INNER(a, b) a##b
#define MOMO_CONCAT(a, b) MOMO_CONCAT_INNER(a, b)
