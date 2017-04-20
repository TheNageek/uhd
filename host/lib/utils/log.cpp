//
// Copyright 2012,2014,2016 Ettus Research LLC
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#include <uhd/utils/log.hpp>
#include <uhd/utils/static.hpp>
#include <uhd/utils/paths.hpp>
#include <uhd/utils/tasks.hpp>
#include <uhd/transport/bounded_buffer.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/make_shared.hpp>
#include <boost/thread/locks.hpp>
#include <boost/thread/mutex.hpp>
#include <fstream>
#include <cctype>
#include <atomic>

namespace pt = boost::posix_time;

static const std::string PURPLE = "\033[35;1m"; // purple
static const std::string BLUE = "\033[34;1m"; // blue
static const std::string GREEN = "\033[32;1m"; // green
static const std::string YELLOW = "\033[33;1m"; // yellow
static const std::string RED = "\033[31;0m"; // red
static const std::string BRED = "\033[31;1m"; // bright red
static const std::string RESET_COLORS = "\033[39;0m"; // reset colors

/***********************************************************************
 * Helpers
 **********************************************************************/
static const std::string verbosity_color(const uhd::log::severity_level &level){
    switch(level){
    case (uhd::log::trace):
        return PURPLE;
    case(uhd::log::debug):
        return BLUE;
    case(uhd::log::info):
        return GREEN;
    case(uhd::log::warning):
        return YELLOW;
    case(uhd::log::error):
        return RED;
    case(uhd::log::fatal):
        return BRED;
    default:
        return RESET_COLORS;
    }
}

//! get the relative file path from the host directory
inline std::string path_to_filename(std::string path)
{
    return path.substr(path.find_last_of("/\\") + 1);
}

/***********************************************************************
 * Logger backends
 **********************************************************************/


void console_log(
    const uhd::log::logging_info &log_info
) {
        std::clog
#ifdef UHD_LOG_CONSOLE_COLOR
            << verbosity_color(log_info.verbosity)
#endif
#ifdef UHD_LOG_CONSOLE_TIME
            << "[" << pt::to_simple_string(log_info.time) << "] "
#endif
#ifdef UHD_LOG_CONSOLE_THREAD
            << "[0x" << log_info.thread_id << "] "
#endif
#ifdef UHD_LOG_CONSOLE_SRC
            << "[" << path_to_filename(log_info.file) << ":" << log_info.line << "] "
#endif
            << "[" << log_info.verbosity << "] "
            << "[" << log_info.component << "] "
#ifdef UHD_LOG_CONSOLE_COLOR
            << RESET_COLORS
#endif
            << log_info.message
            << std::endl
            ;
}

/*! Helper class to implement file logging
 *
 * The class holds references to the file stream object, and handles closing
 * and cleanup.
 */
class file_logger_backend
{
public:
    file_logger_backend(const std::string &file_path)
    {
        _file_stream.exceptions(std::ofstream::failbit | std::ofstream::badbit);
        if (!file_path.empty()){
            try {
                _file_stream.open(file_path.c_str(), std::fstream::out | std::fstream::app);
            } catch (const std::ofstream::failure& fail){
                std::cerr << "Error opening log file: " << fail.what() << std::endl;
            }
        }

    }

    void log(const uhd::log::logging_info &log_info)
    {
        if (_file_stream.is_open()){
            _file_stream
                << pt::to_simple_string(log_info.time) << ","
                << "0x" << log_info.thread_id << ","
                << path_to_filename(log_info.file) << ":" << log_info.line << ","
                << log_info.verbosity << ","
                << log_info.component << ","
                << log_info.message
                << std::endl;
            ;
        }
    }


    ~file_logger_backend()
    {
        if (_file_stream.is_open()){
            _file_stream.close();
        }
    }

private:
    std::ofstream _file_stream;
};

/***********************************************************************
 * Global resources for the logger
 **********************************************************************/

#define UHD_CONSOLE_LOGGER_KEY "console"
#define UHD_FILE_LOGGER_KEY "file"

class log_resource {
public:
    uhd::log::severity_level global_level;
    std::map<std::string, uhd::log::severity_level> logger_level;

    log_resource(void):
        global_level(uhd::log::off),
        _exit(false),
        _log_queue(10)
    {
        //allow override from macro definition
#ifdef UHD_LOG_MIN_LEVEL
        this->global_level = _get_log_level(BOOST_STRINGIZE(UHD_LOG_MIN_LEVEL), this->global_level);
#endif
       //allow override from environment variables
        const char * log_level_env = std::getenv("UHD_LOG_LEVEL");
        if (log_level_env != NULL && log_level_env[0] != '\0') {
            this->global_level =
                _get_log_level(log_level_env, this->global_level);
        }


        /***** Console logging ***********************************************/
#ifndef UHD_LOG_CONSOLE_DISABLE
        uhd::log::severity_level console_level = uhd::log::trace;
#ifdef UHD_LOG_CONSOLE_LEVEL
        console_level = _get_log_level(BOOST_STRINGIZE(UHD_LOG_CONSOLE_LEVEL), console_level);
#endif
        const char * log_console_level_env = std::getenv("UHD_LOG_CONSOLE_LEVEL");
        if (log_console_level_env != NULL && log_console_level_env[0] != '\0') {
            console_level =
                _get_log_level(log_console_level_env, console_level);
        }
        logger_level[UHD_CONSOLE_LOGGER_KEY] = console_level;
        _loggers[UHD_CONSOLE_LOGGER_KEY] = &console_log;
#endif

        /***** File logging **************************************************/
        uhd::log::severity_level file_level = uhd::log::trace;
        std::string log_file_target;
#if defined(UHD_LOG_FILE_LEVEL) && defined(UHD_LOG_FILE_PATH)
        file_level = _get_log_level(BOOST_STRINGIZE(UHD_LOG_FILE_LEVEL), file_level);
        log_file_target = BOOST_STRINGIZE(UHD_LOG_FILE);
#endif
        const char * log_file_level_env = std::getenv("UHD_LOG_FILE_LEVEL");
        if (log_file_level_env != NULL && log_file_level_env[0] != '\0'){
            file_level = _get_log_level(log_file_level_env, file_level);
        }
        const char* log_file_env = std::getenv("UHD_LOG_FILE");
        if ((log_file_env != NULL) && (log_file_env[0] != '\0')) {
            log_file_target = std::string(log_file_env);
        }
        if (!log_file_target.empty()){
            logger_level[UHD_FILE_LOGGER_KEY] = file_level;
            auto F = boost::make_shared<file_logger_backend>(log_file_target);
            _loggers[UHD_FILE_LOGGER_KEY] = [F](const uhd::log::logging_info& log_info){F->log(log_info);};
        }

        // Launch log message consumer
        _pop_task = uhd::task::make([this](){this->pop_task();});

    }

    ~log_resource(void){
        _exit = true;
        _pop_task.reset();
    }

    void push(const uhd::log::logging_info& log_info)
    {
        _log_queue.push_with_haste(log_info);
    }

    void pop_task()
    {
        while (!_exit) {
            uhd::log::logging_info log_info;
            if (_log_queue.pop_with_timed_wait(log_info, 1)){
                for (const auto &logger : _loggers) {
                    auto level = logger_level.find(logger.first);
                    if(level != logger_level.end() && log_info.verbosity < level->second){
                        continue;
                    }
                    logger.second(log_info);
                }
            }
        }

        // Exit procedure: Clear the queue
        uhd::log::logging_info log_info;
        while (_log_queue.pop_with_haste(log_info)) {
            for (const auto &logger : _loggers) {
                auto level = logger_level.find(logger.first);
                if (level != logger_level.end() && log_info.verbosity < level->second){
                    continue;
                }
                logger.second(log_info);
            }
        }
    }

    void add_logger(const std::string &key, uhd::log::log_fn_t logger_fn)
    {
        _loggers[key] = logger_fn;
    }

private:
    uhd::task::sptr _pop_task;
    uhd::log::severity_level _get_log_level(const std::string &log_level_str,
                                            const uhd::log::severity_level &previous_level){
        if (std::isdigit(log_level_str[0])) {
            const uhd::log::severity_level log_level_num =
                uhd::log::severity_level(std::stoi(log_level_str));
            if (log_level_num >= uhd::log::trace and
                log_level_num <= uhd::log::fatal) {
                return log_level_num;
            }else{
                UHD_LOGGER_ERROR("LOG") << "Failed to set log level to: " << log_level_str;
                return previous_level;
            }
        }

#define if_loglevel_equal(name)                                 \
        else if (log_level_str == #name) return uhd::log::name
        if_loglevel_equal(trace);
        if_loglevel_equal(debug);
        if_loglevel_equal(info);
        if_loglevel_equal(warning);
        if_loglevel_equal(error);
        if_loglevel_equal(fatal);
        if_loglevel_equal(off);
        return previous_level;
    }

    std::atomic<bool> _exit;
    std::map<std::string, uhd::log::log_fn_t> _loggers;
    uhd::transport::bounded_buffer<uhd::log::logging_info> _log_queue; // Init auf size 10 oder so
};

UHD_SINGLETON_FCN(log_resource, log_rs);

/***********************************************************************
 * The logger object implementation
 **********************************************************************/

uhd::_log::log::log(
    const uhd::log::severity_level verbosity,
    const std::string &file,
    const unsigned int line,
    const std::string &component,
    const boost::thread::id thread_id
    ) :
    _log_it(verbosity >= log_rs().global_level)
{
    if (_log_it){
        this->_log_info = uhd::log::logging_info(
            pt::microsec_clock::local_time(),
            verbosity,
            file,
            line,
            component,
            thread_id);
        }
}

uhd::_log::log::~log(void)
{
    if (_log_it) {
        this->_log_info.message = _ss.str();
        log_rs().push(this->_log_info);
    }
}


void
uhd::log::add_logger(const std::string &key, log_fn_t logger_fn)
{
    log_rs().add_logger(key, logger_fn);
}

void
uhd::log::set_log_level(uhd::log::severity_level level){
    log_rs().global_level = level;
}

void
uhd::log::set_logger_level(const std::string &key, uhd::log::severity_level level){
    log_rs().logger_level[key] = level;
}

void
uhd::log::set_console_level(uhd::log::severity_level level){
    set_logger_level(UHD_CONSOLE_LOGGER_KEY, level);
}

void
uhd::log::set_file_level(uhd::log::severity_level level){
    set_logger_level(UHD_FILE_LOGGER_KEY, level);
}

