# Checks for recovery_min_apply_delay
use strict;
use warnings;
use PostgresNode;
use TestLib;
use Test::More tests => 11;

sub PostgresNode::psql_ok {
   my ($self, $sql, $comment) = @_;

   $self->command_ok(['psql', '-A', '-t', '--no-psqlrc',
   	'-d', $self->connstr, '-c', $sql], $comment);
}

sub PostgresNode::psql_fails {
   my ($self, $sql, $comment) = @_;

   $self->command_ok(['psql', '-A', '-t', '--no-psqlrc',
   	'-d', $self->connstr, '-c', $sql], $comment);
}

# Setup master node
my $node_master = get_new_node("Candie");
$node_master->init(allows_streaming => 1);
$node_master->append_conf('postgresql.conf', qq(
max_prepared_transactions = 10
));
$node_master->start;
$node_master->backup('master_backup');
$node_master->psql('postgres', "create table t(id int)");

# Setup master node
my $node_slave = get_new_node('Django');
$node_slave->init_from_backup($node_master, 'master_backup', has_streaming => 1);
$node_slave->start;

# Switch to synchronous replication
$node_master->append_conf('postgresql.conf', qq(
synchronous_standby_names = '*'
));
$node_master->restart;

###############################################################################
# Check that we can commit and abort after soft restart.
# Here checkpoint happens before shutdown and no WAL replay will not occur
# during start. So code should re-create memory state from files.
###############################################################################

$node_master->psql('postgres', "
	begin;
	insert into t values (42);
	prepare transaction 'x';
	begin;
	insert into t values (142);
	prepare transaction 'y';");
$node_master->stop;
$node_master->start;
$node_master->psql_ok("commit prepared 'x'",
	'Commit prepared tx after restart.');
$node_master->psql_ok("rollback prepared 'y'",
	'Rollback prepared tx after restart.');

###############################################################################
# Check that we can commit and abort after hard restart.
# On startup WAL replay will re-create memory for global transactions that 
# happend after last checkpoint and stored.  
###############################################################################

$node_master->psql('postgres', "
	begin;
	insert into t values (42);
	prepare transaction 'x';
	begin;
	insert into t values (142);
	prepare transaction 'y';");
$node_master->teardown_node;
$node_master->start;
$node_master->psql_ok("commit prepared 'x'",
	'Commit prepared tx after teardown.');
$node_master->psql_ok("rollback prepared 'y'",
	'Rollback prepared tx after teardown.');

###############################################################################
# Check that we can replay several tx with same name.
###############################################################################

$node_master->psql('postgres', "
	begin;
	insert into t values (42);
	prepare transaction 'x';
	commit prepared 'x';
	begin;
	insert into t values (42);
	prepare transaction 'x';");
$node_master->teardown_node;
$node_master->start;
$node_master->psql_ok("commit prepared 'x'",
	'Check that we can replay several tx with same name.');

###############################################################################
# Check that WAL replay will cleanup it's memory state and release locks while 
# replaying commit.
###############################################################################

$node_master->psql('postgres', "
	begin;
	insert into t values (42);
	prepare transaction 'x';
	commit prepared 'x';");
$node_master->teardown_node;
$node_master->start;
$node_master->psql_ok("
	begin;
	insert into t values (42);
	-- This prepare can fail due to 2pc identifier or locks conflicts if replay
	-- didn't cleanup proc, gxact and locks on commit.
	prepare transaction 'x';",
	"Check that WAL replay will cleanup it's memory state");

###############################################################################
# Check that we can commit while running active sync slave and that there is no
# active prepared transaction on slave after that.
###############################################################################

$node_master->psql('postgres', "
	begin;
	insert into t values (42);
	prepare transaction 'x';
	commit prepared 'x';
	");
my $result = $node_slave->psql('postgres', "select * from pg_prepared_xacts;");
is($result, "", "Check that WAL replay will cleanup it's memory state on slave");

###############################################################################
# The same as in previous case, but let's force checkpoint on slave between
# prepare and commit.
###############################################################################

$node_master->psql('postgres', "
	begin;
	insert into t values (42);
	prepare transaction 'x';
	");
$node_slave->psql('postgres',"checkpoint;");
$node_master->psql('postgres', "commit prepared 'x';");
$result = $node_slave->psql('postgres', "select * from pg_prepared_xacts;");
is($result, "", "Check that WAL replay will cleanup it's memory state on slave after checkpoint");

###############################################################################
# Check that we can commit transaction on promoted slave.
###############################################################################

$node_master->psql('postgres', "
	begin;
	insert into t values (42);
	prepare transaction 'x';
	");
$node_master->teardown_node;
$node_slave->promote;
$node_slave->poll_query_until('postgres', "SELECT pg_is_in_recovery() <> true");
$node_slave->psql_ok("commit prepared 'x';",
	"Check that we can commit transaction on promoted slave.");

# change roles
($node_master, $node_slave) = ($node_slave, $node_master);
$node_slave->enable_streaming($node_master);
$node_slave->append_conf('recovery.conf', qq(
recovery_target_timeline='latest'
));
$node_slave->start;

###############################################################################
# Check that we restore prepared xacts after slave soft restart while master is
# down.
###############################################################################

$node_master->psql('postgres', "
	begin;
	insert into t values (42);
	prepare transaction 'x';
	");
$node_master->stop;
$node_slave->restart;
$node_slave->promote;
$node_slave->poll_query_until('postgres', "SELECT pg_is_in_recovery() <> true");
my $prepared_count = $node_slave->psql('postgres',"select count(*) from pg_prepared_xacts");
is($prepared_count, '1', "Check that we restore prepared xacts after slave soft restart while master is down.");

# restore state
($node_master, $node_slave) = ($node_slave, $node_master);
$node_slave->enable_streaming($node_master);
$node_slave->append_conf('recovery.conf', qq(
recovery_target_timeline='latest'
));
$node_slave->start;
$node_master->psql('postgres',"commit prepared 'x'");

###############################################################################
# Check that we restore prepared xacts after slave hard restart while master is
# down.
###############################################################################

$node_master->psql('postgres', "
	begin;
	insert into t values (242);
	prepare transaction 'x';
	");
$node_master->stop;
$node_slave->teardown_node;
$node_slave->start;
$node_slave->promote;
$node_slave->poll_query_until('postgres', "SELECT pg_is_in_recovery() <> true");
$prepared_count = $node_slave->psql('postgres',"select count(*) from pg_prepared_xacts");
is($prepared_count, '1', "Check that we restore prepared xacts after slave hard restart while master is down.");

# restore state
($node_master, $node_slave) = ($node_slave, $node_master);
$node_slave->enable_streaming($node_master);
$node_slave->append_conf('recovery.conf', qq(
recovery_target_timeline='latest'
));
$node_slave->start;
$node_master->psql('postgres',"commit prepared 'x'");


