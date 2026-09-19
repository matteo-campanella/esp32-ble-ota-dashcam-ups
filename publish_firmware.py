Import('env')

import requests
import os
import sys
from os.path import basename
try:
    import paramiko
    from scp import SCPClient
except ImportError:
    env.Execute("$PYTHONEXE -m pip install paramiko scp")

# from platformio import util
# project_config = util.load_project_config()
# bintray_config = {k: v for k, v in project_config.items("bintray")}
# version = project_config.get("common", "release_version")

try:
    import configparser
except ImportError:
    import ConfigParser as configparser

project_config = configparser.ConfigParser()
project_config.read("platformio.ini")
version = project_config.get("common", "version")
progname = project_config.get("common", "name")
publish_config = {k: v for k, v in project_config.items("publish")}

def publish_firmware(source, target, env):
    firmware_path = str(target[0])
    firmware_name = basename(firmware_path)

    print("Uploading {0} to publish server. Version: {1}".format(firmware_name, version))
    ssh = paramiko.SSHClient()
    key = paramiko.RSAKey.from_private_key_file('C:/Users/matte/.ssh/id_rsa')
    ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    ssh.connect(hostname = publish_config['host'],username = 'pi', pkey = key)
    scp = SCPClient(ssh.get_transport())
    scp.put(firmware_path,publish_config['path'])
    
    scp.close()
    ssh.close()

    os.remove(firmware_path)
    
env.Replace(PROGNAME="%s" % progname)
env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", publish_firmware)