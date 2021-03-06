package org.greenplum.pxf.api.configuration;

import lombok.Getter;
import lombok.Setter;
import org.springframework.boot.autoconfigure.task.TaskExecutionProperties;
import org.springframework.boot.context.properties.ConfigurationProperties;

/**
 * Configuration properties for PXF.
 */
@ConfigurationProperties(prefix = PxfServerProperties.PROPERTY_PREFIX)
public class PxfServerProperties {

    public static final String PXF_BASE_PROPERTY = "pxf.base";

    /**
     * The property prefix for all properties in this group.
     */
    public static final String PROPERTY_PREFIX = "pxf";

    /**
     * The path for the server configuration. If the configuration has not
     * been initialized, it will be set to NOT_INITIALIZED.
     */
    @Getter
    private String base;

    /**
     * Customizable settings for tomcat through PXF
     */
    @Getter
    @Setter
    private Tomcat tomcat = new Tomcat();

    /**
     * Configurable task execution properties for async tasks (i.e Bridge Read)
     */
    @Getter
    @Setter
    private TaskExecutionProperties task = new TaskExecutionProperties();

    @Getter
    @Setter
    public static class Tomcat {

        /**
         * Maximum number of headers allowed in the request
         */
        private int maxHeaderCount = 30000;
    }

    public void setBase(String base) {
        this.base = base;
        System.setProperty(PXF_BASE_PROPERTY, base);
    }
}
