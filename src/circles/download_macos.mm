// Files fetched over HTTPS by macOS's URL loading, in the background.
#include <circles/app.h>

#import <Foundation/Foundation.h>

namespace circles {

namespace {

// Downloads files[index], then the ones after it.
void download_from(std::shared_ptr<const std::vector<DownloadFile>> files,
                   size_t                                         index,
                   std::shared_ptr<std::atomic<i32>>              finished) {
  if (index == files->size())
    return;
  const DownloadFile& file = (*files)[index];
  NSURL* destination = [NSURL fileURLWithPath:@(file.path.c_str())];
  NSURLSessionDownloadTask* task = [NSURLSession.sharedSession
      downloadTaskWithURL:[NSURL URLWithString:@(file.url.c_str())]
        completionHandler:^(NSURL* location, NSURLResponse* response,
                            NSError* error) {
          // A whole file, saved; anything else leaves the path as it was.
          if (!error &&
              static_cast<NSHTTPURLResponse*>(response).statusCode == 200) {
            NSFileManager* manager = NSFileManager.defaultManager;
            [manager removeItemAtURL:destination error:nil];
            [manager moveItemAtURL:location toURL:destination error:nil];
          }
          ++*finished;
          download_from(files, index + 1, finished);
        }];
  [task resume];
}

}  // namespace

std::shared_ptr<std::atomic<i32>> download_files(
    std::vector<DownloadFile> files) {
  auto finished = std::make_shared<std::atomic<i32>>(0);
  download_from(
      std::make_shared<const std::vector<DownloadFile>>(std::move(files)), 0,
      finished);
  return finished;
}

}  // namespace circles
