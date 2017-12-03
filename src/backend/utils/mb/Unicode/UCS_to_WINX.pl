#! /usr/bin/perl
#
# Copyright (c) 2001-2005, PostgreSQL Global Development Group
#
# $PostgreSQL: pgsql/src/backend/utils/mb/Unicode/UCS_to_WINX.pl,v 1.6 2005/03/07 04:30:52 momjian Exp $
#
# Generate UTF-8 <--> WINX code conversion tables from
# map files provided by Unicode organization.
# Unfortunately it is prohibited by the organization
# to distribute the map files. So if you try to use this script,
# you have to obtain "8859-[2-5].TXT" from the organization's ftp site.
# We assume the file include three tab-separated columns:
#		 ISO/IEC 8859 code in hex
#		 UCS-2 code in hex
#		 # and Unicode name (not used in this script)

require "ucs2utf.pl";
%filename = ('WIN1256'=>'CP1256.TXT',
             'WIN1258'=>'CP1258.TXT',
             'WIN874'=>'CP874.TXT',
             'WIN1250'=>'CP1250.TXT');
@charsets = ('WIN1256','WIN1258','WIN874','WIN1250');
foreach $charset (@charsets) {

#
# first, generate UTF8->ISO8859 table
#
    $in_file = $filename{$charset};

    open( FILE, $in_file ) || die( "cannot open $in_file" );

	reset 'array';

    while( <FILE> ){
		chop;
		if( /^#/ ){
			next;
		}
		( $c, $u, $rest ) = split;
		$ucs = hex($u);
		$code = hex($c);
		if( $code >= 0x80){
			$utf = &ucs2utf($ucs);
			if( $array{ $utf } ne "" ){
				printf STDERR "Warning: duplicate UTF8: %04x\n",$ucs;
				next;
			}
			$count++;
			$array{ $utf } = $code;
		}
	}
    close( FILE );

	$file = lower("utf8_to_${charset}.map");
    open( FILE, "> $file" ) || die( "cannot open $file" );
	print FILE "static pg_utf_to_local ULmap_${charset}[ $count ] = {\n";

	for $index ( sort {$a <=> $b} keys( %array ) ){
		$code = $array{ $index };
		$count--;
		if( $count == 0 ){
			printf FILE "  {0x%04x, 0x%04x}\n", $index, $code;
		} else {
			printf FILE "  {0x%04x, 0x%04x},\n", $index, $code;
		}
	}

	print FILE "};\n";
	close(FILE);

#
# then generate ISO885->UTF8 table
#
    open( FILE, $in_file ) || die( "cannot open $in_file" );

	reset 'array';

    while( <FILE> ){
		chop;
		if( /^#/ ){
			next;
		}
		( $c, $u, $rest ) = split;
		$ucs = hex($u);
		$code = hex($c);
		if($code >= 0x80){
			$utf = &ucs2utf($ucs);
			if( $array{ $utf } ne "" ){
				printf STDERR "Warning: duplicate UTF8: %04x\n",$ucs;
				next;
			}
			$count++;
			$array{ $code } = $utf;
		}
	}
    close( FILE );

	$file = lower("${charset}_to_utf8.map");
    open( FILE, "> $file" ) || die( "cannot open $file" );
	print FILE "static pg_local_to_utf LUmap${charset}[ $count ] = {\n";
	for $index ( sort {$a <=> $b} keys( %array ) ){
		$utf = $array{ $index };
		$count--;
		if( $count == 0 ){
			printf FILE "  {0x%04x, 0x%04x}\n", $index, $utf;
		} else {
			printf FILE "  {0x%04x, 0x%04x},\n", $index, $utf;
		}
	}

	print FILE "};\n";
	close(FILE);
}
