package org.greenplum.pxf.plugins.hive;

import org.apache.hadoop.hive.metastore.IMetaStoreClient;
import org.greenplum.pxf.api.model.Metadata;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.mockito.Mockito.mock;

public class HiveClientWrapperTest {

    private Metadata.Item tblDesc;
    private HiveClientWrapper hiveClientWrapper;

    @BeforeEach
    public void setup() {
        HiveClientWrapper.HiveClientFactory factory = mock(HiveClientWrapper.HiveClientFactory.class);
        hiveClientWrapper = new HiveClientWrapper();
        hiveClientWrapper.setHiveClientFactory(factory);
    }

    @Test
    public void parseTableQualifiedNameNoDbName() {
        String name = "orphan";
        tblDesc = hiveClientWrapper.extractTableFromName(name);

        assertEquals("default", tblDesc.getPath());
        assertEquals(name, tblDesc.getName());
    }

    @Test
    public void parseTableQualifiedName() {
        String name = "not.orphan";
        tblDesc = hiveClientWrapper.extractTableFromName(name);

        assertEquals("not", tblDesc.getPath());
        assertEquals("orphan", tblDesc.getName());
    }

    @Test
    public void parseTableQualifiedNameEmpty() {
        String name = "";
        String errorMsg = "empty string is not a valid Hive table name. "
                + "Should be either <table_name> or <db_name.table_name>";

        parseTableQualifiedNameNegative(name, errorMsg, "empty string");

        name = null;
        parseTableQualifiedNameNegative(name, errorMsg, "null string");

        name = ".";
        errorMsg = surroundByQuotes(name) + " is not a valid Hive table name. "
                + "Should be either <table_name> or <db_name.table_name>";
        parseTableQualifiedNameNegative(name, errorMsg, "empty db and table names");

        name = " . ";
        errorMsg = surroundByQuotes(name) + " is not a valid Hive table name. "
                + "Should be either <table_name> or <db_name.table_name>";
        parseTableQualifiedNameNegative(name, errorMsg, "only white spaces in string");
    }

    @Test
    public void parseTableQualifiedNameTooManyQualifiers() {
        String name = "too.many.parents";
        String errorMsg = surroundByQuotes(name) + " is not a valid Hive table name. "
                + "Should be either <table_name> or <db_name.table_name>";

        parseTableQualifiedNameNegative(name, errorMsg, "too many qualifiers");
    }

    @Test
    public void invalidTableName() {

        IMetaStoreClient metaStoreClient = mock(IMetaStoreClient.class);
        Exception e = assertThrows(IllegalArgumentException.class,
                () -> hiveClientWrapper.extractTablesFromPattern(metaStoreClient, "t.r.o.u.b.l.e.m.a.k.e.r"));
        assertEquals("\"t.r.o.u.b.l.e.m.a.k.e.r\" is not a valid Hive table name. Should be either <table_name> or <db_name.table_name>", e.getMessage());
    }

    private void parseTableQualifiedNameNegative(String name, String errorMsg, String reason) {
        Exception e = assertThrows(IllegalArgumentException.class,
                () -> hiveClientWrapper.extractTableFromName(name),
                "test should fail because of " + reason);
        assertEquals(errorMsg, e.getMessage());
    }

    private String surroundByQuotes(String str) {
        return "\"" + str + "\"";
    }
}
