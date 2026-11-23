#pragma once

#include <Foundation/Strings/StringView.h>
#include <Foundation/Types/Status.h>

/// \brief Loads NVIDIA's signed sl.interposer.dll at run time and resolves the Streamline core API from it.
namespace plStreamlineLoader
{
  /// \brief Loads sl.interposer.dll from the directory after checking its NVIDIA signature.
  plResult Load(plStringView sDirectory);
  void Unload();
  bool IsLoaded();
} // namespace plStreamlineLoader
