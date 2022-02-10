#!/usr/bin/env python
# -*- coding: utf-8 -*-
import time
from collections import OrderedDict 

import pytest
from hamcrest import assert_that, equal_to, greater_than, not_none, none, has_item, has_items, raises, empty, instance_of

from sqs_matchers import ReadResponseMatcher 

from sqs_test_base import KikimrSqsTestBase, get_test_with_sqs_installation_by_path, get_test_with_sqs_tenant_installation, IS_FIFO_PARAMS 
from sqs_test_base import to_bytes
from ydb import issues as ydb_issues
 
 
class QueuesManagingTest(KikimrSqsTestBase): 
    @classmethod 
    def _setup_config_generator(cls): 
        config_generator = super(QueuesManagingTest, cls)._setup_config_generator() 
        config_generator.yaml_config['sqs_config']['account_settings_defaults'] = {'max_queues_count': 10}
        return config_generator 
 
    @pytest.mark.parametrize(**IS_FIFO_PARAMS) 
    def test_create_queue(self, is_fifo):
        attributes = {} 
        if is_fifo:
            self.queue_name = self.queue_name + '.fifo'
            attributes['ContentBasedDeduplication'] = 'true' 
        attributes['DelaySeconds'] = '506' 
        attributes['MaximumMessageSize'] = '10003' 
        attributes['MessageRetentionPeriod'] = '502000' 
        attributes['ReceiveMessageWaitTimeSeconds'] = '11' 
        attributes['VisibilityTimeout'] = '42' 
        created_queue_url = self._create_queue_and_assert(self.queue_name, is_fifo=is_fifo, use_http=True) 
        existing_queues = self._sqs_api.list_queues()
        assert_that(
            created_queue_url in existing_queues
        )
        got_queue_url = self._sqs_api.get_queue_url(self.queue_name)
        assert_that(
            got_queue_url, equal_to(created_queue_url)
        )

        created_attributes = self._sqs_api.get_queue_attributes(got_queue_url) 
        assert_that(equal_to(created_attributes.get('DelaySeconds')), attributes['DelaySeconds']) 
        assert_that(equal_to(created_attributes.get('MaximumMessageSize')), attributes['MaximumMessageSize']) 
        assert_that(equal_to(created_attributes.get('MessageRetentionPeriod')), attributes['MessageRetentionPeriod']) 
        assert_that(equal_to(created_attributes.get('ReceiveMessageWaitTimeSeconds')), attributes['ReceiveMessageWaitTimeSeconds']) 
        assert_that(equal_to(created_attributes.get('VisibilityTimeout')), attributes['VisibilityTimeout']) 
        if is_fifo: 
            assert_that(created_attributes.get('ContentBasedDeduplication'), 'true') 
 
    def test_create_fifo_queue_wo_postfix(self):
        def call_create(): 
            self.called = True 
            self._sqs_api.create_queue(self.queue_name, is_fifo=True) 
 
        assert_that(
            call_create, 
            raises(
                RuntimeError,
                pattern='failed with status 400.*\n.*FIFO queue should end with &quot;\\.fifo&quot;' 
            )
        )

    def test_create_queue_generates_event(self):
        pytest.skip("Outdated")
        self._create_queue_and_assert(self.queue_name, is_fifo=False)
        table_path = '{}/.Queues'.format(self.sqs_root)
        assert_that(self._get_table_lines_count(table_path), equal_to(1))

        table_path = '{}/.Events'.format(self.sqs_root)
        assert_that(self._get_table_lines_count(table_path), equal_to(1))

    def test_remove_queue_generates_event(self):
        pytest.skip("Outdated")
        queue_url = self._create_queue_and_assert(self.queue_name)
        table_path = '{}/.Events'.format(self.sqs_root)
        lines_count = self._get_table_lines_count(table_path)
        assert_that(lines_count, greater_than(0))

        self._sqs_api.delete_queue(queue_url)
        assert_that(self._get_table_lines_count(table_path), greater_than(lines_count))

    def test_create_queue_with_invalid_name(self): 
        def call_create(): 
            self._sqs_api.create_queue('invalid_queue_name!') 
 
        assert_that( 
            call_create, 
            raises( 
                RuntimeError, 
                pattern='Invalid queue name' 
            ) 
        ) 
 
    @pytest.mark.parametrize(**IS_FIFO_PARAMS) 
    def test_delete_queue(self, is_fifo):
        if is_fifo:
            self.queue_name = self.queue_name + '.fifo'
        created_queue_url = self._create_queue_and_assert(self.queue_name, is_fifo=is_fifo)
        self._sqs_api.list_queues()
        self._sqs_api.send_message(created_queue_url, 'body', group_id='group' if is_fifo else None, deduplication_id='123' if is_fifo else None) 
 
        send_message_labels = { 
            'subsystem': 'core', 
            'user': self._username, 
            'queue': self.queue_name, 
            'sensor': 'SendMessage_Count', 
        } 
        counters = self._get_sqs_counters() 
        sends = self._get_counter_value(counters, send_message_labels) 
        assert_that(sends, equal_to(1)) 
 
        delete_result = self._sqs_api.delete_queue(created_queue_url)
        assert_that(
            delete_result, not_none()
        )

        existing_queues = self._sqs_api.list_queues()
        assert_that(
            created_queue_url not in existing_queues,
            "Deleted queue appears in list_queues()"
        )

        time.sleep(2) 
        counters = self._get_sqs_counters() 
        sends = self._get_counter_value(counters, send_message_labels) 
        assert_that(sends, none()) 
 
        def describe_queue_path(): 
            self._driver.scheme_client.describe_path('{}/{}/{}'.format(self.sqs_root, self._username, self.queue_name))
 
        assert_that( 
            describe_queue_path, 
            raises( 
                ydb_issues.SchemeError 
            ) 
        ) 
 
    def test_delete_queue_batch(self): 
        existing_queues = self._sqs_api.list_queues() 
        assert_that( 
            existing_queues, empty() 
        ) 
        url1 = self._create_queue_and_assert('{}_1.fifo'.format(self.queue_name), is_fifo=True) 
        url2 = self._create_queue_and_assert('{}_2'.format(self.queue_name), is_fifo=False) 
        url3 = self._create_queue_and_assert('{}_3'.format(self.queue_name), is_fifo=False) 
        url4 = to_bytes(url2) + to_bytes('_incorrect_url')
        existing_queues = [to_bytes(y) for y in self._sqs_api.list_queues()]
        assert_that( 
            len(existing_queues), equal_to(3) 
        ) 
        assert_that( 
            existing_queues, has_items(to_bytes(url1), to_bytes(url2))
        ) 
 
        delete_queue_batch_result = self._sqs_api.private_delete_queue_batch([url1, url2, url4]) 
        assert_that( 
            delete_queue_batch_result['DeleteQueueBatchResultEntry'], instance_of(list) 
        ) 
        assert_that( 
            len(delete_queue_batch_result['DeleteQueueBatchResultEntry']), equal_to(2) 
        ) 
        assert_that( 
            delete_queue_batch_result['BatchResultErrorEntry'], instance_of(OrderedDict)  # that means that we have only one entry for error 
        ) 
 
        existing_queues = [to_bytes(y) for y in self._sqs_api.list_queues()]
        assert_that( 
            len(existing_queues), equal_to(1) 
        ) 
        assert_that( 
            existing_queues, has_item(to_bytes(url3))
        ) 
 
    @pytest.mark.parametrize(**IS_FIFO_PARAMS) 
    def test_purge_queue(self, is_fifo): 
        if is_fifo: 
            self.queue_name = self.queue_name + '.fifo' 
        created_queue_url = self._create_queue_and_assert(self.queue_name, is_fifo=is_fifo) 
        if is_fifo: 
            group_id = 'group' 
        else: 
            group_id = None 
        self._send_messages(created_queue_url, 10, self._msg_body_template, is_fifo=is_fifo, group_id=group_id) 
        self._sqs_api.purge_queue(created_queue_url) 
 
        tries = 20 
        while tries: 
            tries -= 1 
            time.sleep(0.5) 
 
            ret = self._read_while_not_empty(created_queue_url, 10, visibility_timeout=0) 
            if len(ret) > 0 and tries: 
                continue 
            assert_that(len(ret), equal_to(0)) 
            break 
 
        self._check_queue_tables_are_empty() 
 
        # We can continue working after purge 
        message_ids = self._send_messages(created_queue_url, 1, self._msg_body_template, is_fifo=is_fifo, group_id=group_id) 
        self._read_messages_and_assert(created_queue_url, 1, matcher=ReadResponseMatcher().with_message_ids(message_ids)) 
 
    def test_purge_queue_batch(self): 
        created_queue_url1 = self._create_queue_and_assert(self.queue_name) 
        created_queue_url2 = self._create_queue_and_assert(self.queue_name + '1') 
        created_queue_url3 = to_bytes(created_queue_url2) + to_bytes('_nonexistent_queue_url')
        self._send_messages(created_queue_url1, 10, self._msg_body_template) 
        self._send_messages(created_queue_url2, 10, self._msg_body_template) 
        purge_queue_batch_result = self._sqs_api.private_purge_queue_batch([created_queue_url1, created_queue_url2, created_queue_url3]) 
        assert_that( 
            purge_queue_batch_result['PurgeQueueBatchResultEntry'], instance_of(list) 
        ) 
        assert_that( 
            len(purge_queue_batch_result['PurgeQueueBatchResultEntry']), equal_to(2) 
        ) 
        assert_that( 
            purge_queue_batch_result['BatchResultErrorEntry'], instance_of(OrderedDict)  # that means that we have only one entry for error 
        ) 
 
        def check_purged_queue(queue_url): 
            tries = 20 
            while tries: 
                tries -= 1 
 
                ret = self._read_while_not_empty(queue_url, 10, visibility_timeout=0) 
                if len(ret) > 0 and tries: 
                    time.sleep(0.5) 
                    continue 
                assert_that(len(ret), equal_to(0)) 
                break 
 
            self._check_queue_tables_are_empty() 
 
        check_purged_queue(created_queue_url1) 
        check_purged_queue(created_queue_url2) 
 
    @pytest.mark.parametrize(**IS_FIFO_PARAMS) 
    def test_delete_and_create_queue(self, is_fifo):
        if is_fifo:
            self.queue_name = self.queue_name + '.fifo'
        created_queue_url = self._create_queue_and_assert(self.queue_name, is_fifo=is_fifo, use_http=True) 
        self.seq_no += 1 
        self._send_message_and_assert(created_queue_url, self._msg_body_template.format(1), seq_no=self.seq_no if is_fifo else None, group_id='group' if is_fifo else None) 
        delete_result = self._sqs_api.delete_queue(created_queue_url)
        assert_that(
            delete_result, not_none()
        )
        created_queue_url = self._create_queue_and_assert(self.queue_name, is_fifo=is_fifo, use_http=True) 
 
        master_is_updated = False 
        for i in range(100):
            try: 
                self._read_messages_and_assert( 
                    created_queue_url, 10, ReadResponseMatcher().with_n_messages(0) 
                ) 
                master_is_updated = True 
                break 
            except RuntimeError as ex: 
                assert str(ex).find('master session error') != -1 
                time.sleep(0.5)  # wait master update time 
 
        assert_that(master_is_updated) 
        self.seq_no += 1 
        msg_id = self._send_message_and_assert(created_queue_url, self._msg_body_template.format(2), seq_no=self.seq_no if is_fifo else None, group_id='group' if is_fifo else None) 
        self._read_messages_and_assert(
            created_queue_url, 10, ReadResponseMatcher().with_message_ids([msg_id, ]) 
        )
 
    def test_ya_count_queues(self):
        assert_that(self._sqs_api.private_count_queues(), equal_to('0'))
        q_url = self._create_queue_and_assert('new_q')
        self._create_queue_and_assert('new_q_2')

        time.sleep(2.1) 
        assert_that(self._sqs_api.private_count_queues(), equal_to('2'))

        self._sqs_api.delete_queue(q_url)

        time.sleep(2.1) 
        assert_that(self._sqs_api.private_count_queues(), equal_to('1'))

    def test_queues_count_over_limit(self): 
        urls = [] 
        for i in range(10):
            urls.append(self._create_queue_and_assert('queue_{}'.format(i), shards=1, retries=1)) 
 
        def call_create(): 
            self._sqs_api.create_queue('extra_queue') 
 
        assert_that( 
            call_create, 
            raises( 
                RuntimeError, 
                pattern='OverLimit' 
            ) 
        ) 
 
        def set_max_queues_count(count): 
            self._execute_yql_query('UPSERT INTO `{}/.Settings` (Account, Name, Value) VALUES (\'{}\', \'MaxQueuesCount\', \'{}\')'
                                    .format(self.sqs_root, self._username, count))
 
        set_max_queues_count(12) 
 
        for i in range(10, 12):
            urls.append(self._create_queue_and_assert('queue_{}'.format(i), shards=1, retries=1)) 
 
        assert_that( 
            call_create, 
            raises( 
                RuntimeError, 
                pattern='OverLimit' 
            ) 
        ) 
 
        self._sqs_api.delete_queue(urls[5]) 
        self._create_queue_and_assert('new_queue', shards=1, retries=1) 
 
        self._sqs_api.delete_queue(urls[1]) 
        self._sqs_api.delete_queue(urls[10]) 
 
        set_max_queues_count(10) 
 
        assert_that( 
            call_create, 
            raises( 
                RuntimeError, 
                pattern='OverLimit' 
            ) 
        ) 
 
 
class TestQueuesManagingWithTenant(get_test_with_sqs_tenant_installation(QueuesManagingTest)): 
    pass 
 
 
class TestQueuesManagingWithPathTestQueuesManagingWithPath(get_test_with_sqs_installation_by_path(QueuesManagingTest)):
    pass 
