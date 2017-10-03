#! /usr/bin/env python

'''
gp_replica_check

Tool to validate replication
'''

import subprocess
import threading

class ReplicaCheck(threading.Thread):
    def __init__(self, row):
        super(ReplicaCheck, self).__init__()
        self.host = row[0]
        self.port = row[1]
        self.datname = row[5]
        self.ploc = row[6]
        self.mloc = row[7]

    def __str__(self):
        return 'Host: %s, Port: %s, Database: %s\n\
Primary Filespace Location: %s\n\
Mirror Filespace Location: %s' % (self.host, self.port, self.datname,
                                          self.ploc, self.mloc)

    def run(self):
        print(self)
        cmd = '''PGOPTIONS='-c gp_session_role=utility' psql -h %s -p %s -c "select * from gp_replica_check('%s', '%s', 'all')" %s''' % (self.host, self.port, self.ploc, self.mloc, self.datname)
        res = subprocess.check_output(cmd, stderr=subprocess.STDOUT, shell=True)
        print res

def get_fsmap():
    sql = '''
SELECT gscp.address,
       gscp.port,
       gscp.content,
       gscp.mode,
       gscp.status,
       pdb.datname,
       CASE WHEN tep.oid = 1663 THEN fep.fselocation || '/' || 'base/' || pdb.oid
            WHEN tep.oid = 1664 THEN fep.fselocation || '/' || 'global'
            ELSE fep.fselocation || '/' || tep.oid
       END AS ploc,
       CASE WHEN tem.oid = 1663 THEN fem.fselocation || '/' || 'base/' || pdb.oid
            WHEN tem.oid = 1664 THEN fem.fselocation || '/' || 'global'
            ELSE fem.fselocation || '/' || tem.oid
       END AS mloc
FROM gp_segment_configuration gscp, pg_filespace_entry fep, pg_tablespace tep,
     gp_segment_configuration gscm, pg_filespace_entry fem, pg_tablespace tem,
     pg_filespace pfs, pg_database pdb
WHERE fep.fsedbid = gscp.dbid
      AND fem.fsedbid = gscm.dbid
      AND ((tep.oid = tem.oid AND tep.oid = 1663) OR (tep.oid = 1664 AND tem.oid = 1664 AND pdb.oid = 12090))
      AND gscp.content = gscm.content
      AND gscp.role = 'p'
      AND gscm.role = 'm'
      AND pfs.oid = fep.fsefsoid
      AND pdb.datname != 'template0'
'''

    fsmap = {}
    a = subprocess.check_output('psql postgres -t -c "%s"' % sql, stderr=subprocess.STDOUT, shell=True).split('\n')
    for ai in a:
        aj = map(str.strip, ai.split('|'))
        if len(aj) > 1:
            fsmap.setdefault(aj[2], []).append(ReplicaCheck(aj))

    return fsmap

def install_extension():
    sql = '''
SELECT datname FROM pg_database WHERE datname != 'template0'
'''
    sql2 = '''
CREATE EXTENSION IF NOT EXISTS gp_replica_check
'''

    a = subprocess.check_output('psql postgres -t -c "%s"' % sql, stderr=subprocess.STDOUT, shell=True).split('\n')
    for ai in a:
        if len(ai) > 1:
            b = subprocess.check_output('psql %s -t -c "%s"' % (ai.strip(), sql2), stderr=subprocess.STDOUT, shell=True)
            print b

def run_checkpoint():
    sql = '''CHECKPOINT'''
    a = subprocess.check_output('psql postgres -t -c "%s"' % sql, stderr=subprocess.STDOUT, shell=True)
    print a

def start_verification(fsmap):
    for content, fsinfo_list in fsmap.items():
        for fsinfo in fsinfo_list:
            fsinfo.start()
            fsinfo.join()

if __name__ == '__main__':
    install_extension()
    run_checkpoint()
    fsmap = get_fsmap()
    start_verification(fsmap)
