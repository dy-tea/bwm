#!/usr/bin/env perl
use strict;
use File::Basename;
use File::Spec;

my $var = shift // 'data';

my $content = '';
$content .= $_ while (<STDIN>);

my $script_dir = dirname(File::Spec->rel2abs($0));
my $shader_dir = $script_dir;

my %processed = ();

sub process_includes {
  my ($text, $dir) = @_;
  my $result = '';

  foreach my $line (split /\n/, $text) {
    if ($line =~ /^\s*#include\s+[<"]([^>"]+)[>"]/) {
      my $include_file = $1;

      if ($processed{$include_file}) {
        $result .= "// #include \"$include_file\" (already included)\n";
        next;
      }

      my $include_path;
      if (defined $dir && -f "$dir/$include_file") {
        $include_path = "$dir/$include_file";
      } elsif (-f $include_file) {
        $include_path = $include_file;
      }

      if (defined $include_path && -f $include_path) {
        $processed{$include_file} = 1;
        open my $fh, '<', $include_path or die "Cannot open $include_path: $!";
        my $inc_content = do { local $/; <$fh> };
        close $fh;

        $result .= "// BEGIN: $include_file\n";
        $result .= process_includes($inc_content, dirname($include_path));
        $result .= "// END: $include_file\n";
      } else {
        $result .= $line . "\n";
      }
    } else {
      $result .= $line . "\n";
    }
  }

  return $result;
}

my $processed_content = process_includes($content, $shader_dir);

print "static const char $var\[\] = {\n";

for my $byte (split //, $processed_content) {
  printf "\t0x%02x,\n", ord($byte);
}

print "\t0x00,\n";
print "};\n";
