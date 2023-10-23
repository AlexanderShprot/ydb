# -*- coding: utf-8 -*-
import logging
import os
import subprocess
import sys

import pytest

from ydb import Driver, DriverConfig, SessionPool
from ydb.tests.library.harness.util import LogLevels
from ydb.tests.library.harness.ydb_fixtures import ydb_database_ctx
from ydb.tests.oss.ydb_sdk_import import ydb

logger = logging.getLogger(__name__)


# local configuration for the ydb cluster (fetched by ydb_cluster_configuration fixture)
CLUSTER_CONFIG = dict(
    additional_log_configs={
        # more logs
        'GRPC_PROXY': LogLevels.DEBUG,
        'GRPC_SERVER': LogLevels.DEBUG,
        'FLAT_TX_SCHEMESHARD': LogLevels.TRACE,
        # less logs
        'KQP_PROXY': LogLevels.DEBUG,
        'KQP_GATEWAY': LogLevels.DEBUG,
        'KQP_WORKER': LogLevels.ERROR,
        'KQP_YQL': LogLevels.ERROR,
        'KQP_SESSION': LogLevels.ERROR,
        'KQP_COMPILE_ACTOR': LogLevels.ERROR,
        'TX_DATASHARD': LogLevels.ERROR,
        'HIVE': LogLevels.ERROR,
        'CMS_TENANTS': LogLevels.ERROR,
        'PERSQUEUE_CLUSTER_TRACKER': LogLevels.CRIT,
        'TX_PROXY_SCHEME_CACHE': LogLevels.CRIT,
        'TX_PROXY': LogLevels.CRIT,
    },
    enable_audit_log=True,
    # extra_feature_flags=['enable_grpc_audit'],
)


def cluster_endpoint(cluster):
    return f'{cluster.nodes[1].host}:{cluster.nodes[1].port}'


def ydbcli_db_schema_exec(cluster, operation_proto):
    endpoint = cluster_endpoint(cluster)
    args = [
        # cluster.binary_path,
        cluster.nodes[1].binary_path,
        f'--server=grpc://{endpoint}',
        'db',
        'schema',
        'exec',
        operation_proto,
    ]
    r = subprocess.run(args, capture_output=True)
    assert r.returncode == 0, r


def alter_database_audit_settings(cluster, database_path, enable_dml_audit, expected_subjects=None):
    alter_proto = r'''ModifyScheme {
        OperationType: ESchemeOpAlterExtSubDomain
        WorkingDir: "%s"
        SubDomain {
            Name: "%s"
            AuditSettings {
                EnableDmlAudit: %s
            }
        }
    }''' % (
        os.path.dirname(database_path),
        os.path.basename(database_path),
        enable_dml_audit,
    )
    ydbcli_db_schema_exec(cluster, alter_proto)


class CaptureFileOutput:
    def __init__(self, filename):
        self.filename = filename

    def __enter__(self):
        self.saved_pos = os.path.getsize(self.filename)
        return self

    def __exit__(self, *exc):
        with open(self.filename, 'r') as f:
            f.seek(self.saved_pos)
            self.captured = f.read()


@pytest.fixture(scope='module')
def _database(ydb_cluster, ydb_root, request):
    database_path = os.path.join(ydb_root, request.node.name)
    with ydb_database_ctx(ydb_cluster, database_path):
        yield database_path


@pytest.fixture(scope='module')
def _client_session_pool_with_auth(ydb_cluster, _database):
    with Driver(DriverConfig(cluster_endpoint(ydb_cluster), _database, auth_token='root@builtin')) as driver:
        with SessionPool(driver) as pool:
            yield pool


@pytest.fixture(scope='module')
def _client_session_pool_no_auth(ydb_cluster, _database):
    with Driver(DriverConfig(cluster_endpoint(ydb_cluster), _database, auth_token=None)) as driver:
        with SessionPool(driver) as pool:
            yield pool


@pytest.fixture(scope='module')
def _client_session_pool_bad_auth(ydb_cluster, _database):
    with Driver(DriverConfig(cluster_endpoint(ydb_cluster), _database, auth_token='__bad__@builtin')) as driver:
        with SessionPool(driver) as pool:
            yield pool


def create_table(pool, table_path):
    def f(s, table_path):
        s.execute_scheme(fr'''
            create table `{table_path}` (
                id int64,
                value int64,
                primary key (id)
            );
        ''')
    pool.retry_operation_sync(f, table_path=table_path, retry_settings=None)


def fill_table(pool, table_path):
    def f(s, table_path):
        s.transaction().execute(fr'''
            insert into `{table_path}` (id, value) values (1, 1), (2, 2)
        ''')
    pool.retry_operation_sync(f, table_path=table_path, retry_settings=None)


@pytest.fixture(scope='module')
def prepared_test_env(ydb_cluster, _database, _client_session_pool_no_auth):
    database_path = _database
    table_path = os.path.join(database_path, 'test-table')
    pool = _client_session_pool_no_auth

    create_table(pool, table_path)
    fill_table(pool, table_path)

    capture_audit = CaptureFileOutput(ydb_cluster.config.audit_file_path)
    print('AAA', capture_audit.filename, file=sys.stderr)
    # print('AAA', ydb_cluster.config.binary_path, file=sys.stderr)

    alter_database_audit_settings(ydb_cluster, database_path, enable_dml_audit=True)

    return table_path, capture_audit


def execute_data_query(pool, text):
    pool.retry_operation_sync(lambda s: s.transaction().execute(text, commit_tx=True))


QUERIES = [
    r'''insert into `{table_path}` (id, value) values (100, 100), (101, 101)''',
    r'''select id from `{table_path}`''',
    r'''update `{table_path}` set value = 0 where id = 1''',
    r'''delete from `{table_path}` where id = 2''',
    r'''replace into `{table_path}` (id, value) values (2, 3), (3, 3)''',
    r'''upsert into `{table_path}` (id, value) values (4, 4), (5, 5)''',
]


@pytest.mark.parametrize("query_template", QUERIES, ids=lambda x: x.split(maxsplit=1)[0])
def test_single_dml_query_logged(query_template, prepared_test_env, _client_session_pool_with_auth):
    table_path, capture_audit = prepared_test_env

    pool = _client_session_pool_with_auth
    query_text = query_template.format(table_path=table_path)

    with capture_audit:
        execute_data_query(pool, query_text)

    print(capture_audit.captured, file=sys.stderr)
    assert query_text in capture_audit.captured


def test_dml_begin_commit_logged(prepared_test_env, _client_session_pool_with_auth):
    table_path, capture_audit = prepared_test_env

    pool = _client_session_pool_with_auth

    with pool.checkout() as session:
        with capture_audit:
            tx = session.transaction().begin()
            tx.execute(fr'''update `{table_path}` set value = 0 where id = 1''')
            tx.commit()

    print(capture_audit.captured, file=sys.stderr)
    assert 'BeginTransaction' in capture_audit.captured
    assert 'CommitTransaction' in capture_audit.captured


# TODO: fix ydbd crash on exit
# def test_dml_begin_rollback_logged(prepared_test_env, _client_session_pool_with_auth):
#     table_path, capture_audit = prepared_test_env
#
#     pool = _client_session_pool_with_auth
#
#     with pool.checkout() as session:
#         with capture_audit:
#             tx = session.transaction().begin()
#             tx.execute(fr'''update `{table_path}` set value = 0 where id = 1''')
#             tx.rollback()
#
#     print(capture_audit.captured, file=sys.stderr)
#     assert 'BeginTransaction' in capture_audit.captured
#     assert 'RollbackTransaction' in capture_audit.captured


def test_dml_requests_arent_logged_when_anonymous(prepared_test_env, _client_session_pool_no_auth):
    table_path, capture_audit = prepared_test_env
    pool = _client_session_pool_no_auth

    with capture_audit:
        for i in QUERIES:
            query_text = i.format(table_path=table_path)
            execute_data_query(pool, query_text)

    print(capture_audit.captured, file=sys.stderr)
    assert len(capture_audit.captured) == 0, capture_audit.captured


def test_dml_requests_logged_when_unauthorized(prepared_test_env, _client_session_pool_bad_auth):
    table_path, capture_audit = prepared_test_env
    pool = _client_session_pool_bad_auth

    for i in QUERIES:
        query_text = i.format(table_path=table_path)
        with pool.checkout() as session:
            tx = session.transaction()
            with capture_audit:
                with pytest.raises(ydb.issues.SchemeError):
                    tx.execute(query_text, commit_tx=True)
            print(capture_audit.captured, file=sys.stderr)
            assert query_text in capture_audit.captured
