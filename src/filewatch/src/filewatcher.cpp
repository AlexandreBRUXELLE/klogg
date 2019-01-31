#include "filewatcher.h"

#include "log.h"
#include "persistentinfo.h"
#include "configuration.h"

#include <efsw/efsw.hpp>
#include <vector>

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QTimer>

namespace {

struct WatchedFile {
    std::string name;
    int64_t mTime;
    int64_t size;

    bool operator==(const std::string& filename) const
    {
        return name == filename;
    }

    bool operator!=(const WatchedFile& other) const
    {
        return name != other.name || mTime != other.mTime || size != other.size;
    }
};

struct WatchedDirecotry {
    efsw::WatchID watchId;

    // filenames are in utf8
    std::string name;
    std::vector<WatchedFile> files;
};

} // namespace

class EfswFileWatcher final : public efsw::FileWatchListener {
  public:
    explicit EfswFileWatcher( FileWatcher* parent )
        : parent_{ parent }
        , mutex_{ QMutex::Recursive }
    {
    }

    void addFile( const QString& fullFileName )
    {
        QMutexLocker lock( &mutex_ );

        LOG( logDEBUG ) << "QtFileWatcher::addFile " << fullFileName.toStdString();

        const QFileInfo fileInfo = QFileInfo( fullFileName );

        auto watchedFile = WatchedFile{ fileInfo.fileName().toStdString(), fileInfo.lastModified().toMSecsSinceEpoch(), fileInfo.size() };

        const auto directory = fileInfo.canonicalPath().toStdString();

        const auto wasEmpty = watchedPaths_.empty();

        auto watchedDirectory
            = std::find_if( watchedPaths_.begin(), watchedPaths_.end(),
                            [&directory]( const auto& wd ) { return wd.name == directory; } );

        if ( watchedDirectory == watchedPaths_.end() ) {
            auto watchId = watcher_.addWatch( directory, this, false );

            if ( watchId < 0 ) {
                LOG( logWARNING ) << "QtFileWatcher::addFile failed to add watch " << directory
                                  << " error " << watchId;
                return;
            }

            WatchedDirecotry wd;
            wd.watchId = watchId;
            wd.name = directory;

            wd.files.emplace_back( std::move( watchedFile ) );

            watchedPaths_.emplace_back( std::move( wd ) );
        }
        else {
            if ( std::find( watchedDirectory->files.begin(), watchedDirectory->files.end(),
                            watchedFile.name )
                 != watchedDirectory->files.end() ) {
                LOG( logDEBUG ) << "QtFileWatcher: already watching " << watchedFile.name << " in "
                                << directory;
                return;
            }

            watchedDirectory->files.emplace_back( std::move( watchedFile ) );
        }

        if ( wasEmpty ) {
            watcher_.watch();
        }
    }

    void removeFile( const QString& fullFileName )
    {
        QMutexLocker lock( &mutex_ );

        LOG( logDEBUG ) << "QtFileWatcher::removeFile " << fullFileName.toStdString();

        const QFileInfo fileInfo = QFileInfo( fullFileName );

        const auto filename = fileInfo.fileName().toStdString();
        const auto directory = fileInfo.absolutePath().toStdString();

        auto watchedDirectory
            = std::find_if( watchedPaths_.begin(), watchedPaths_.end(),
                            [&directory]( const auto& wd ) { return wd.name == directory; } );

        if ( watchedDirectory != watchedPaths_.end() ) {

            auto watchedFile = std::find( watchedDirectory->files.begin(),
                                          watchedDirectory->files.end(), filename );

            if ( watchedFile != watchedDirectory->files.end() ) {
                watchedDirectory->files.erase( watchedFile );
            }

            if ( watchedDirectory->files.empty() ) {
                watcher_.removeWatch( watchedDirectory->watchId );
                watchedPaths_.erase( watchedDirectory );
            }
        }
        else {
            LOG( logWARNING ) << "QtFileWatcher::removeFile - The file is not watched!";
        }

        for ( const auto& d : watcher_.directories() ) {
            LOG( logERROR ) << "Directories still watched: " << d;
        }
    }

    void checkWatches()
    {
        const auto collectChangedFiles = [this]()
        {
            QMutexLocker lock( &mutex_ );

            std::vector<QString> changedFiles;

            for ( auto& dir : watchedPaths_ ) {
                for ( auto& file : dir.files ) {
                    const auto path = QDir::cleanPath( QString::fromStdString( dir.name ) + QDir::separator()
                                                + QString::fromStdString( file.name ) );

                    const auto fileInfo = QFileInfo{ path };

                    auto watchedFile = WatchedFile{ fileInfo.fileName().toStdString(), fileInfo.lastModified().toMSecsSinceEpoch(), fileInfo.size() };

                    if ( file != watchedFile ) {
                        changedFiles.push_back( path );
                        LOG( logINFO ) << "QtFileWatcher::checkWatches - will notify for " << path;
                    }

                    file = std::move( watchedFile );
                }
            }

            return changedFiles;
        };

        for ( const auto& changedFile : collectChangedFiles() ) {
            QMetaObject::invokeMethod( parent_, "fileChangedOnDisk", Qt::QueuedConnection,
                                                    Q_ARG( QString, changedFile ) );
        }
    }

    void handleFileAction( efsw::WatchID watchid, const std::string& dir,
                           const std::string& filename, efsw::Action action,
                           std::string oldFilename ) override
    {
        Q_UNUSED( watchid );
        Q_UNUSED( action );

        auto qtDir = QString::fromStdString( dir );
        if ( qtDir.endsWith( QDir::separator() ) ) {
            qtDir.chop( 1 );
        }

        const auto directory = qtDir.toStdString();

        LOG( logDEBUG ) << "QtFileWatcher::fileChangedOnDisk " << directory << " " << filename
                        << ", old name " << oldFilename;

        const auto fullChangedFilename = [&]() {
            QMutexLocker lock( &mutex_ );

            auto watchedDirectory
                = std::find_if( watchedPaths_.begin(), watchedPaths_.end(),
                                [&directory]( const auto& wd ) { return wd.name == directory; } );

            if ( watchedDirectory != watchedPaths_.end() ) {
                std::string changedFilename;

                const auto isFileWatched
                    = std::any_of( watchedDirectory->files.begin(), watchedDirectory->files.end(),
                                   [&filename, &oldFilename, &changedFilename]( const auto& f ) {
                                       if ( f.name == filename || f.name == oldFilename ) {
                                           changedFilename = f.name;
                                           return true;
                                       }

                                       return false;
                                   } );

                if ( isFileWatched ) {
                    LOG( logINFO ) << "QtFileWatcher::fileChangedOnDisk - will notify for "
                                   << filename << ", old name " << oldFilename;

                    return QDir::cleanPath( QString::fromStdString( directory ) + QDir::separator()
                                            + QString::fromStdString( changedFilename ) );
                }
                else {
                    LOG( logDEBUG )
                        << "QtFileWatcher::fileChangedOnDisk - call but no file monitored";
                }
            }
            else {
                LOG( logDEBUG ) << "QtFileWatcher::fileChangedOnDisk - call but no dir monitored";
            }

            return QString{};
        }();

        if ( !fullChangedFilename.isEmpty() ) {
            QMetaObject::invokeMethod( parent_, "fileChangedOnDisk", Qt::QueuedConnection,
                                       Q_ARG( QString, fullChangedFilename ) );
        }
    }

  private:
    efsw::FileWatcher watcher_;
    std::vector<WatchedDirecotry> watchedPaths_;
    FileWatcher* parent_;

    QMutex mutex_;
};

void EfswFileWatcherDeleter::operator()( EfswFileWatcher* watcher ) const
{
    delete watcher;
}

FileWatcher::FileWatcher()
    : checkTimer_ { new QTimer() }
    , efswWatcher_{ new EfswFileWatcher( this ) }
{
    connect( checkTimer_.get(), &QTimer::timeout, this, &FileWatcher::checkWatches );
}

FileWatcher::~FileWatcher() {}

FileWatcher& FileWatcher::getFileWatcher()
{
    static auto* const instance = new FileWatcher;
    return *instance;
}

void FileWatcher::addFile( const QString& fileName )
{
    efswWatcher_->addFile( fileName );

    setPolling();
}

void FileWatcher::removeFile( const QString& fileName )
{
    efswWatcher_->removeFile( fileName );

    setPolling();
}

void FileWatcher::fileChangedOnDisk( const QString& fileName )
{
    emit fileChanged( fileName );
}

void FileWatcher::setPolling()
{
 const auto config = Persistent<Configuration>( "settings" );

    if ( config->pollingEnabled() ) {
        LOG( logINFO ) << "Polling files enabled";
        checkTimer_->start( config->pollIntervalMs() );
    }
    else {
        LOG( logINFO ) << "Polling files disabled";
        checkTimer_->stop();
    }
}

void FileWatcher::checkWatches( )
{
    efswWatcher_->checkWatches();
}
