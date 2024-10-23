#include <unistd.h>
#include <linux/limits.h>
#include <assert.h>
#include <sys/mman.h>
#include <fcntl.h>

#include <string>
#include <fstream>
#include <iostream>
#include <filesystem>
#include <algorithm>
#include <cctype>
#include <locale>
#include <vector>

using namespace std;

#define DECONTAINERIZR_FS_ROOT "DECONTAINERIZR_FS_ROOT"

static void warn(const string &msg) {
    cerr << "WARN: " << msg << endl;
}
static void error(const string &msg) {
    cerr << "ERROR: " << msg << endl
 ;
}static void dump_args(const char* name, char *const args[]) {
    for (int idx = 0; args[idx] != NULL; ++idx) {
        printf("%s[%d] %s\n", name, idx, args[idx]);
    }
}

static bool is_target_under(const filesystem::path &target, const filesystem::path &base) {
    for (auto bit = base.begin(), tit = target.begin(); bit != base.end(); ++bit, ++tit) {
        if (tit == target.end() || *bit != *tit) {
            return false;
        }
    }
    return true;
}

static filesystem::path resolve_target(const filesystem::path &short_name, const filesystem::path &fs_root) {
    return short_name.is_absolute()
        ? is_target_under(filesystem::weakly_canonical(short_name), fs_root)
            ? short_name
            : resolve_target(fs_root / short_name.relative_path(), fs_root)
        : resolve_target(filesystem::absolute(short_name), fs_root);
}
/**
 * Resolves short name into decontainerizr filesystem
 */
static filesystem::path resolve_target(const string &short_name, const filesystem::path &fs_root, const string &path) {
    if (short_name.empty()) {
        throw (new string("unable to resolve empty target"));
    }

    if (short_name.rfind('/', 0) == 0 || short_name.rfind("./", 0) == 0 || short_name.rfind("../", 0) == 0) {
        return resolve_target((filesystem::path) short_name, fs_root);
    } else {
        stringstream path_stream(path);
        string path_element; while (getline(path_stream, path_element, ':')) {
            string real_exe = path_element + "/" + short_name;
            if (access(real_exe.c_str(), X_OK) != -1) {
                return real_exe;
            }
        }
        throw (new string("unable to resolve "))->append(short_name);
    }
}

static void lock_execution(filesystem::path &self_exe) {
    flock l = {
        .l_type = F_WRLCK,
        .l_whence = SEEK_SET,
        .l_start = 0,
        .l_len = 0,
        .l_pid = 0
    };
    assert(
        fcntl(
            shm_open(("/decontainerizr-" + to_string(hash<filesystem::path>{}(self_exe))).c_str(), O_RDWR | O_CREAT, 0660),
            F_SETLKW,
            &l
        ) != -1
    );
}

static void set_shebang_interpreter(const filesystem::path &script_path, filesystem::path &interpreter_path) {
    auto new_script_length = ({
        fstream script(script_path);

        auto start_pos = script.tellg();
        string script_payload = ({ stringstream buf; buf << script.rdbuf(); buf.str(); });

        size_t shebang_params_offset = 0;
        if (script_payload.rfind("#!", 0) == 0) {
            size_t shebang_length = script_payload.find('\n');
            shebang_length = shebang_length == string::npos ? script_payload.length() : shebang_length;

            shebang_params_offset = script_payload.find(' ');
            shebang_params_offset = shebang_params_offset == string::npos || shebang_params_offset > shebang_length
                ? shebang_length
                : shebang_params_offset;
        }

        script.seekg(start_pos);
        script
            << ("#!" + interpreter_path.string())
            << (shebang_params_offset == 0 ? "\n" : "")
            << (&script_payload.c_str()[shebang_params_offset]);
        script.tellg() - start_pos;
    });

    filesystem::resize_file(script_path, new_script_length);
}
static void relocate_scripts(filesystem::path &intp_path, filesystem::path &cfg_dir, filesystem::path &fs_root) {
    ifstream relocatable_scripts(cfg_dir / "relocatable_scripts");

    string target_script; while (relocatable_scripts.is_open() && getline(relocatable_scripts, target_script)) {
        if (target_script.empty()) {
            continue;
        }
        if (target_script[0] == '/') {
            warn("unable to relocate " + target_script + ", must be relative");
            continue;
        }

        auto target_script_path = fs_root / target_script;
        if (!filesystem::exists(target_script_path)) {
            warn("unable to relocate " + target_script + ", is absent");
            continue;
        }

        set_shebang_interpreter(target_script_path, intp_path);
    }
}

static string replace_all(const string_view &source, const string_view &from, const string_view &to) {
    string target; target.reserve(source.length());

    for (size_t left = 0, right = source.find(from, left); left < source.length(); right = source.find(from, left)) {
        right = right == string::npos ? source.length() : right;

        target.append(source, left, right - left);
        if (right < source.length()) {
            target.append(to);
            right += from.length();
        }
        left = right;
    }

    return target;
}
static void prepare_environment(filesystem::path &root, filesystem::path &fs_root, filesystem::path &cfg_dir) {
    {
        ifstream env_vars(cfg_dir / "env_vars");
        string line; while (getline(env_vars, line, (char) 0)) {
            if (line.empty()) {
                continue;
            }
            auto equal_pos = line.find('=');
            if (equal_pos == string::npos) {
                warn("unable to process env var " + line);
                continue;
            }
            auto name = line.substr(0, equal_pos);
            auto value = replace_all(line.substr(equal_pos + 1), "$DECONTAINERIZR_FS_ROOT", fs_root.c_str());

            if (name == "PATH") {
                value = (root / "bin").string() + ({
                    stringstream path_stream(value); string new_path;
                    string path_element; while (getline(path_stream, path_element, ':')) {
                        ((new_path += ":") += fs_root) += path_element;
                    }
                    new_path + ":" + getenv("PATH");
                });
            }

            setenv(name.c_str(), value.c_str(), true);
        }
    }

    setenv(DECONTAINERIZR_FS_ROOT, fs_root.c_str(), true);
}

static inline string& ltrim(string &s) {
    s.erase(
        s.begin(),
        std::find_if(s.begin(), s.end(), std::not1(std::ptr_fun<int, int>(std::isspace)))
    );
    return s;
}
static inline string& rtrim(string &s) {
    s.erase(
        std::find_if(s.rbegin(), s.rend(), std::not1(std::ptr_fun<int, int>(std::isspace))).base(),
        s.end()
    );
    return s;
}
static inline string trim(string s) {
    return ltrim(rtrim(s));
}

int main(const int argc, char *const argv[]) {
    // dump_args("argv", argv);

    auto self_exe = resolve_target((string) argv[0], "/", getenv("PATH"));
    auto self_dir = ({ auto p = getenv("DECONTAINERIZR_ROOT_OVERRIDE"); p != NULL ? p : self_exe.parent_path().parent_path(); }); // todo need to use proper file location and symlink

    auto cfg_dir = self_dir / "cfg";
    auto fs_root = self_dir / "fs_root";

    if (getenv("DECONTAINERIZR_FS_ROOT") == NULL) { 
        lock_execution(self_dir);

        relocate_scripts(self_exe, cfg_dir, fs_root);
        prepare_environment(self_dir, fs_root, cfg_dir);
    }

    auto parse_exec_request = [](char pfx[], size_t pfx_sz, string &request) {
        pfx_sz -= 1;
    
        auto arg_start = ({ auto p = request.find(' ', pfx_sz); p == string::npos ? request.length() : p; });

        return make_tuple(trim(request.substr(pfx_sz, arg_start - pfx_sz)), trim(request.substr(arg_start)));
    };
    auto to_heap = [](string &&str) {
        return (new string(str))->data();
    };

    string exec_request = argv[1]; vector<char *> exec_args; filesystem::path target_exe{};
    if (char pfx[] = "shebang_exec "; exec_request.rfind(pfx, 0) == 0) { // shebang_exec <abs_path> <arg>?
        auto args = parse_exec_request(pfx, sizeof pfx, exec_request);

        target_exe = resolve_target(get<0>(args), fs_root, getenv("PATH"));
        exec_args.push_back(to_heap(target_exe));
        if (!get<1>(args).empty()) {
            exec_args.push_back((new string(get<1>(args)))->data());
        }
        exec_args.push_back(argv[2]);
    } else if (char pfx[] = "ld_exec "; exec_request.rfind(pfx, 0) == 0) { // ld_exec <ld> <rel_path>
        auto args = parse_exec_request(pfx, sizeof pfx, exec_request);

        target_exe = resolve_target(get<0>(args), fs_root, getenv("PATH"));

        auto absolute_target = resolve_target((string) argv[2], fs_root, getenv("PATH"));
        exec_args.push_back(to_heap(target_exe));
        exec_args.push_back(to_heap("--argv0")); exec_args.push_back(to_heap(absolute_target));
        exec_args.push_back(to_heap("--library-path")); exec_args.push_back(getenv("DECONTAINERIZR_LD_LIBRARY_PATH"));

        exec_args.push_back(to_heap(filesystem::canonical(absolute_target).parent_path() / get<1>(args)));
    } else if (exec_request.rfind("static_exec ", 0) == 0) { // direct_exec <rel_path>
        // TODO
    } else {
        error("unknown exec_type " + (string) argv[1]);
        return 1;
    }
    for (int idx = 3; idx < argc; ++idx) {
        exec_args.push_back(argv[idx]);
    }
    exec_args.push_back(NULL);

    // cout << "intp: " << target_exe.c_str() << endl; dump_args("exec_argv", &exec_args[0]);

    printf("failed to start target, error: %d\n", (execv(target_exe.c_str(), &exec_args[0]), errno));
    return 1;
}