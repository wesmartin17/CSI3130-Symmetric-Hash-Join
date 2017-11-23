# Test behavior with different schema on subscriber
use strict;
use warnings;
use PostgresNode;
use TestLib;
use Test::More tests => 3;

sub wait_for_caught_up
{
	my ($node, $appname) = @_;

	$node->poll_query_until('postgres',
"SELECT pg_current_wal_lsn() <= replay_lsn FROM pg_stat_replication WHERE application_name = '$appname';"
	) or die "Timed out while waiting for subscriber to catch up";
}

# Create publisher node
my $node_publisher = get_new_node('publisher');
$node_publisher->init(allows_streaming => 'logical');
$node_publisher->start;

# Create subscriber node
my $node_subscriber = get_new_node('subscriber');
$node_subscriber->init(allows_streaming => 'logical');
$node_subscriber->start;

# Create some preexisting content on publisher
$node_publisher->safe_psql('postgres',
	"CREATE TABLE test_tab (a int primary key, b varchar)");
$node_publisher->safe_psql('postgres',
	"INSERT INTO test_tab VALUES (1, 'foo'), (2, 'bar')");

# Setup structure on subscriber
$node_subscriber->safe_psql('postgres', "CREATE TABLE test_tab (a int primary key, b text, c timestamptz DEFAULT now(), d bigint DEFAULT 999)");

# Setup logical replication
my $publisher_connstr = $node_publisher->connstr . ' dbname=postgres';
$node_publisher->safe_psql('postgres', "CREATE PUBLICATION tap_pub FOR TABLE test_tab");

my $appname = 'tap_sub';
$node_subscriber->safe_psql('postgres',
"CREATE SUBSCRIPTION tap_sub CONNECTION '$publisher_connstr application_name=$appname' PUBLICATION tap_pub"
);

wait_for_caught_up($node_publisher, $appname);

# Also wait for initial table sync to finish
my $synced_query =
"SELECT count(1) = 0 FROM pg_subscription_rel WHERE srsubstate NOT IN ('r', 's');";
$node_subscriber->poll_query_until('postgres', $synced_query)
  or die "Timed out while waiting for subscriber to synchronize data";

my $result =
  $node_subscriber->safe_psql('postgres', "SELECT count(*), count(c), count(d = 999) FROM test_tab");
is($result, qq(2|2|2), 'check initial data was copied to subscriber');

# Update the rows on the publisher and check the additional columns on
# subscriber didn't change
$node_publisher->safe_psql('postgres', "UPDATE test_tab SET b = md5(b)");

wait_for_caught_up($node_publisher, $appname);

$result =
  $node_subscriber->safe_psql('postgres', "SELECT count(*), count(c), count(d = 999) FROM test_tab");
is($result, qq(2|2|2), 'check extra columns contain local defaults');

# Change the local values of the extra columns on the subscriber,
# update publisher, and check that subscriber retains the expected
# values
$node_subscriber->safe_psql('postgres', "UPDATE test_tab SET c = 'epoch'::timestamptz + 987654321 * interval '1s'");
$node_publisher->safe_psql('postgres', "UPDATE test_tab SET b = md5(a::text)");

wait_for_caught_up($node_publisher, $appname);

$result =
  $node_subscriber->safe_psql('postgres', "SELECT count(*), count(extract(epoch from c) = 987654321), count(d = 999) FROM test_tab");
is($result, qq(2|2|2), 'check extra columns contain locally changed data');

$node_subscriber->stop;
$node_publisher->stop;
