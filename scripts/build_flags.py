Import("env")
import datetime
import os
import subprocess
import sys

def format_string_define(key, value):
    return "'" + f"-D {key}=\"" + str(value).strip() + "\"'"
def format_define(key, value):
    return f"-D {key}={value}"
def format_define_bool(key, value):
    bool_string = "false"
    if value:
        bool_string = "true"
    return format_define(key, bool_string)
def get_git_version():
    git_version_string = "ver unknown"
    git_version_result = subprocess.run("git describe --all --dirty", capture_output=True, encoding='utf-8', shell=True)
    if git_version_result.returncode == 0:
        git_version_string = git_version_result.stdout
    return git_version_string
def get_current_time():
    return datetime.datetime.now().isoformat()
def get_is_cloud_build():
    return os.getenv("GITHUB_ACTIONS") == "true"

build_flags = env.get('BUILD_FLAGS', [])
build_flags.append(format_string_define("VERSION", get_git_version()))
build_flags.append(format_string_define("BUILDTIME", get_current_time()))
build_flags.append(format_define_bool("CLOUD_BUILD", get_is_cloud_build()))
env['BUILD_FLAGS'] = build_flags
sys.stdout.write("\nbuild flags: %s\n\n" % build_flags)