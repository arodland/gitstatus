// Copyright 2019 Roman Perepelitsa.
//
// This file is part of GitStatus.
//
// GitStatus is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// GitStatus is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with GitStatus. If not, see <https://www.gnu.org/licenses/>.

#include "index.h"

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <stack>

#include "algorithm.h"
#include "check.h"
#include "dir.h"
#include "index.h"
#include "port.h"
#include "scope_guard.h"
#include "stat.h"
#include "thread_pool.h"

namespace gitstatus {

namespace {

void CommonDir(const char* a, const char* b, size_t* dir_len, size_t* dir_depth) {
  *dir_len = 0;
  *dir_depth = 0;
  for (size_t i = 1; *a == *b && *a; ++i, ++a, ++b) {
    if (*a == '/') {
      *dir_len = i;
      ++*dir_depth;
    }
  }
}

size_t Weight(const IndexDir& dir) { return 1 + dir.subdirs.size() + dir.files.size(); }

mode_t Mode(mode_t mode) {
  if (S_ISREG(mode)) {
    mode_t perm = mode & 0111 ? 0755 : 0644;
    return S_IFREG | perm;
  }
  return mode & S_IFMT;
}

bool IsModified(const git_index_entry* entry, const struct stat& st) {
  return entry->mtime.seconds != MTim(st).tv_sec || entry->mtime.nanoseconds != MTim(st).tv_nsec ||
         entry->ino != st.st_ino || entry->mode != Mode(st.st_mode) || entry->gid != st.st_gid ||
         entry->file_size != st.st_size;
}

// TODO: Make me pretty, or at least not fucking ugly.
std::vector<const char*> ScanDirs(git_index* index, int root_fd, IndexDir* const* begin,
                                  IndexDir* const* end, bool untracked_cache) {
  std::string scratch;
  scratch.reserve(4 << 10);
  std::vector<size_t> entries;
  entries.reserve(128);
  std::vector<const char*> res;

  int dir_fd = -1;
  ON_SCOPE_EXIT(&) {
    if (dir_fd >= 0) CHECK(!close(dir_fd));
  };

  for (; begin != end; ++begin) {
    IndexDir& dir = **begin;

    auto Basename = [&](const git_index_entry* e) { return e->path + dir.path.len; };

    auto AddUnmached = [&](StringView basename) {
      if (!basename.len) {
        dir.st = {};
        dir.arena.clear();
        dir.unmatched.clear();
      } else {
        if (basename.len == 5 && !std::memcmp(basename.ptr, ".git/", 5)) return;
      }
      dir.unmatched.push_back(dir.arena.size());
      dir.arena.append(dir.path.ptr, dir.path.len);
      dir.arena.append(basename.ptr, basename.len);
      dir.arena += '\0';
    };

    ON_SCOPE_EXIT(&) {
      for (size_t p : dir.unmatched) res.push_back(&dir.arena[p]);
    };

    if (dir_fd >= 0 && begin[-1]->depth + 1 == dir.depth) {
      CHECK(dir.path.StartsWith(begin[-1]->path));
      scratch.assign(dir.path.ptr + begin[-1]->path.len, dir.path.ptr + dir.path.len - 1);
      int fd = openat(dir_fd, scratch.c_str(), kNoATime | O_RDONLY | O_DIRECTORY | O_CLOEXEC);
      CHECK(!close(dir_fd));
      dir_fd = fd;
    } else {
      if (dir.path.len) {
        CHECK(dir.path.ptr[0] != '/');
        CHECK(dir.path.ptr[dir.path.len - 1] == '/');
        scratch.assign(dir.path.ptr, dir.path.len - 1);
      } else {
        scratch = ".";
      }
      if (dir_fd >= 0) CHECK(!close(dir_fd));
      dir_fd = openat(root_fd, scratch.c_str(), kNoATime | O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    }
    if (dir_fd < 0) {
      AddUnmached("");
      continue;
    }

    struct stat st;
    if (fstat(dir_fd, &st)) {
      AddUnmached("");
      continue;
    }

    if (untracked_cache && StatEq(st, dir.st)) {
      for (const git_index_entry* file : dir.files) {
        struct stat st;
        if (fstatat(dir_fd, Basename(file), &st, AT_SYMLINK_NOFOLLOW)) st = {};
        if (IsModified(file, st)) res.push_back(file->path);  // modified
      }
    } else {
      if (!ListDir(dir_fd, scratch, entries)) {
        AddUnmached("");
        continue;
      }
      dir.st = st;
      dir.arena.clear();
      dir.unmatched.clear();

      std::sort(entries.begin(), entries.end(),
                [&](size_t a, size_t b) { return std::strcmp(&scratch[a], &scratch[b]) < 0; });
      const git_index_entry* const* file = dir.files.data();
      const git_index_entry* const* file_end = file + dir.files.size();
      const StringView* subdir = dir.subdirs.data();
      const StringView* subdir_end = subdir + dir.subdirs.size();

      for (size_t p : entries) {
        StringView entry = &scratch[p];
        bool matched = false;

        for (; file != file_end; ++file) {
          int cmp = Cmp(Basename((*file)), entry);
          if (cmp < 0) {
            res.push_back((*file)->path);  // deleted
          } else if (cmp == 0) {
            if (git_index_entry_newer_than_index(*file, index)) {
              res.push_back((*file)->path);  // racy
            } else {
              struct stat st;
              if (fstatat(dir_fd, entry.ptr, &st, AT_SYMLINK_NOFOLLOW)) st = {};
              if (IsModified(*file, st)) res.push_back((*file)->path);  // modified
            }
            matched = true;
            ++file;
            break;
          } else {
            break;
          }
        }

        if (matched) continue;

        for (; subdir != subdir_end; ++subdir) {
          int cmp = Cmp(*subdir, entry);
          if (cmp > 0) break;
          if (cmp == 0) {
            matched = true;
            ++subdir;
            break;
          }
        }

        if (!matched) {
          if (entry.ptr[-1] == DT_DIR) scratch[p + entry.len++] = '/';
          AddUnmached(entry);  // new
        }
      }
    }
  }

  return res;
}

}  // namespace

Index::Index(const char* root_dir, git_index* index)
    : dirs_(&arena_), splits_(&arena_), git_index_(index), root_dir_(root_dir) {
  size_t total_weight = InitDirs(index);
  InitSplits(total_weight);
}

size_t Index::InitDirs(git_index* index) {
  const size_t index_size = git_index_entrycount(index);
  dirs_.reserve(index_size / 8);
  std::stack<IndexDir*> stack;
  stack.push(arena_.DirectInit<IndexDir>(&arena_));

  size_t total_weight = 0;
  auto PopDir = [&] {
    CHECK(!stack.empty());
    IndexDir* top = stack.top();
    CHECK(top->depth + 1 == stack.size());
    if (!std::is_sorted(top->subdirs.begin(), top->subdirs.end())) Sort(top->subdirs);
    total_weight += Weight(*top);
    dirs_.push_back(top);
    stack.pop();
  };

  for (size_t i = 0; i != index_size; ++i) {
    const git_index_entry* entry = git_index_get_byindex(index, i);
    IndexDir* prev = stack.top();
    size_t common_len, common_depth;
    CommonDir(prev->path.ptr, entry->path, &common_len, &common_depth);
    CHECK(common_depth <= prev->depth);

    for (size_t i = common_depth; i != prev->depth; ++i) PopDir();

    for (const char* p = entry->path + common_len; (p = std::strchr(p, '/')); ++p) {
      IndexDir* top = stack.top();
      StringView subdir(entry->path + top->path.len, p);
      top->subdirs.push_back(subdir);
      IndexDir* dir = arena_.DirectInit<IndexDir>(&arena_);
      dir->path = StringView(entry->path, p - entry->path + 1);
      dir->depth = stack.size();
      CHECK(dir->path.ptr[dir->path.len - 1] == '/');
      stack.push(dir);
    }

    CHECK(!stack.empty());
    IndexDir* dir = stack.top();
    dir->files.push_back(entry);
  }

  CHECK(!stack.empty());
  do {
    PopDir();
  } while (!stack.empty());
  std::reverse(dirs_.begin(), dirs_.end());

  return total_weight;
}

void Index::InitSplits(size_t total_weight) {
  constexpr size_t kMinShardWeight = 512;
  const size_t kNumShards = 16 * GlobalThreadPool()->num_threads();
  const size_t shard_weight = std::max(kMinShardWeight, total_weight / kNumShards);

  splits_.reserve(kNumShards + 1);
  splits_.push_back(0);

  for (size_t i = 0, w = 0; i != dirs_.size(); ++i) {
    w += Weight(*dirs_[i]);
    if (w >= shard_weight) {
      w = 0;
      splits_.push_back(i + 1);
    }
  }

  if (splits_.back() != dirs_.size()) splits_.push_back(dirs_.size());
  CHECK(splits_.size() <= kNumShards + 1);
  CHECK(std::is_sorted(splits_.begin(), splits_.end()));
  CHECK(std::adjacent_find(splits_.begin(), splits_.end()) == splits_.end());
}

void Index::GetDirtyCandidates(ArenaVector<const char*>& candidates, bool untracked_cache) {
  int root_fd = open(root_dir_, kNoATime | O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  VERIFY(root_fd >= 0);
  ON_SCOPE_EXIT(&) { CHECK(!close(root_fd)); };

  std::mutex mutex;
  std::condition_variable cv;
  size_t inflight = splits_.size() - 1;
  bool error = false;
  candidates.clear();

  for (size_t i = 0; i != splits_.size() - 1; ++i) {
    size_t from = splits_[i];
    size_t to = splits_[i + 1];

    auto F = [&, from, to]() {
      ON_SCOPE_EXIT(&) {
        std::unique_lock<std::mutex> lock(mutex);
        CHECK(inflight);
        if (--inflight == 0) cv.notify_one();
      };
      try {
        std::vector<const char*> c =
            ScanDirs(git_index_, root_fd, dirs_.data() + from, dirs_.data() + to, untracked_cache);
        if (!c.empty()) {
          std::unique_lock<std::mutex> lock(mutex);
          candidates.insert(candidates.end(), c.begin(), c.end());
        }
      } catch (const Exception&) {
        std::unique_lock<std::mutex> lock(mutex);
        error = true;
      }
    };

    if (i == splits_.size() - 2) {
      F();
    } else {
      GlobalThreadPool()->Schedule(std::move(F));
    }
  }

  {
    std::unique_lock<std::mutex> lock(mutex);
    while (inflight) cv.wait(lock);
  }

  VERIFY(!error);
  Sort(candidates);
}

}  // namespace gitstatus